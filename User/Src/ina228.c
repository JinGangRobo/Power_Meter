/* ina228.c —— 功率计芯片驱动

   读一个寄存器两步走: 先发寄存器地址, 再收 3 字节, 都用中断(_IT),
   发完地址在中断里接着发起接收, 收完置标志。
   中断里只置标志, 解码和滤波放在 INA228_Task() 里做(浮点在主循环算)。

   VBUS/CURRENT/POWER 每 600µs 连读一遍, 功率直接取芯片算好的 POWER。
*/
#include "ina228.h"
#include "i2c.h"
#include "app.h"          /* 抢总线/释放总线 */

/* ---- 中断和主循环都要用的变量, 要 volatile ---- */
static volatile uint8_t tx_is_read;    /* 这次发的是"读"的前半段, 发完要接着收 */
static volatile uint8_t rx_len;        /* 要收几个字节 */
static volatile uint8_t rx_reg;        /* 正在读哪个寄存器 */
static volatile uint8_t io_result;     /* 0=没完, 1=成功, 2=失败(只有初始化会等它) */
static volatile uint8_t data_pending;  /* 收到数据了, 等 Task 解码 */

/* 收发缓冲放全局: 中断里 HAL 还在用它, 放局部变量函数一返回就没了 */
static uint8_t txbuf[3];
static uint8_t rxbuf[3];               /* 收 3 字节, 读 DEVICE_ID 只用前 2 个 */

/* ---- 连读进度 ---- */
static uint8_t  burst_idx;             /* 这一轮下一个读哪个寄存器 */
static uint8_t  burst_active;          /* 这一轮读着没 */
static uint32_t next_set_us;           /* 下一轮什么时候开始(µs) */
static uint32_t burst_deadline_us;     /* 这一轮的截止时刻, 超时就作废重来 */
static const uint8_t burst_regs[3] =
{
  INA228_REG_VBUS,
  INA228_REG_CURRENT,
  INA228_REG_POWER
};

/* ---- 测量结果 ---- */
static volatile uint8_t comm_ok;       /* 通信状态(有 ACK 即正常) */
static volatile uint8_t cal_pending;   /* 校准完成标志, 等 app 取走 */
static volatile float  filt_v;         /* 滤波后电压 */
static volatile float  filt_i_raw;     /* 滤波后电流原始值(未扣偏移) */
static volatile float  filt_p;         /* 滤波后功率(来自 POWER 寄存器) */
static float  i_offset;                /* 零点偏移 */
static uint16_t dev_id;                /* 自检读回的 DEVICE_ID */

/* ---- V/I/P 环形滤波缓冲, 存未扣偏移的原始值 ---- */
static float  vbuf[INA228_FILTER_SAMPLES];
static float  ibuf[INA228_FILTER_SAMPLES];
static float  pbuf[INA228_FILTER_SAMPLES];
static float  vsum, isum, psum;
static uint8_t vcount, icount, pcount;
static uint8_t vidx, iidx, pidx;

/* ======================= 内部函数 ======================= */

/* 压入电压样本, 更新滤波均值(vcount 恒 >0, 不会除零) */
static void INA228_PushV(float v)
{
  vsum += v - vbuf[vidx];
  vbuf[vidx] = v;
  vidx = (uint8_t)((vidx + 1u) & (INA228_FILTER_SAMPLES - 1u));
  if (vcount < INA228_FILTER_SAMPLES)
  {
    vcount++;
  }
  filt_v = vsum / (float)vcount;
}

/* 压入电流样本并更新滤波均值 */
static void INA228_PushI(float i)
{
  isum += i - ibuf[iidx];
  ibuf[iidx] = i;
  iidx = (uint8_t)((iidx + 1u) & (INA228_FILTER_SAMPLES - 1u));
  if (icount < INA228_FILTER_SAMPLES)
  {
    icount++;
  }
  filt_i_raw = isum / (float)icount;
}

/* 压入功率样本, 更新滤波均值 */
static void INA228_PushP(float p)
{
  psum += p - pbuf[pidx];
  pbuf[pidx] = p;
  pidx = (uint8_t)((pidx + 1u) & (INA228_FILTER_SAMPLES - 1u));
  if (pcount < INA228_FILTER_SAMPLES)
  {
    pcount++;
  }
  filt_p = psum / (float)pcount;
}

/* 解码一次读回的数据(主循环里调用, 不在中断里) */
static void INA228_Decode(uint8_t reg, const uint8_t *b)
{
  if (reg == INA228_REG_VBUS)
  {
    uint32_t raw = ((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8) | (uint32_t)b[2];
    /* VBUS: 高 20 位无符号, >>4 后乘 LSB(195.3125µV) */
    float v = (float)(raw >> 4) * (float)(INA228_VBUS_LSB_UV * 1e-6);
    INA228_PushV(v);
    comm_ok = 1u;                       /* 读成功, 恢复通信状态 */
  }
  else if (reg == INA228_REG_CURRENT)
  {
    uint32_t raw = ((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8) | (uint32_t)b[2];
    /* CURRENT: 20 位有符号, 符号位在 bit23;
       先符号扩展到 32 位再 /16(低 4 位恒为 0, 除得尽), 然后乘电流 LSB */
    int32_t s = (int32_t)((raw & 0x800000u) ? (raw | 0xFF000000u) : raw);
    s /= 16;
    float a = (float)s * INA228_CURRENT_LSB_A;
    INA228_PushI(a);
    comm_ok = 1u;
  }
  else if (reg == INA228_REG_POWER)
  {
    uint32_t raw = ((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8) | (uint32_t)b[2];
    /* POWER: 整个 24 位都是有符号值(跟 VBUS/CURRENT 的 20 位不同, 不能 >>4!),
       直接乘 POWER LSB(3.2 x 电流 LSB)就是瓦数 */
    int32_t s = (int32_t)((raw & 0x800000u) ? (raw | 0xFF000000u) : raw);
    float p = (float)s * INA228_POWER_LSB_W;
    INA228_PushP(p);
    comm_ok = 1u;
  }
  else if (reg == INA228_REG_DEVICE_ID)
  {
    /* DEVICE_ID 是 16 位寄存器, 只比前 2 字节。
       判定放宽: 有应答就算通信正常, ID 值只存下来供开机打印 */
    dev_id  = (uint16_t)(((uint16_t)b[0] << 8) | (uint16_t)b[1]);
    comm_ok = 1u;
  }
  else
  {
    /* 未知寄存器, 忽略 */
  }
}

/* 启动一次寄存器读: 先抢总线, 发 1 字节寄存器地址;
 * TxCplt 回调里接着发起接收, 全程不释放总线 */
static uint8_t INA228_KickRead(uint8_t reg, uint8_t nbytes)
{
  if (I2C_TryAcquire(I2C_OWNER_INA228) == 0u)
  {
    return 0u;                          /* 总线被占, 本次跳过 */
  }
  rx_reg     = reg;
  rx_len     = nbytes;
  tx_is_read = 1u;
  io_result  = 0u;
  txbuf[0]   = reg;
  if (HAL_I2C_Master_Transmit_IT(&hi2c1, (uint16_t)(INA228_I2C_ADDR << 1),
                                 txbuf, 1u) != HAL_OK)
  {
    I2C_Release();
    io_result = 2u;
    return 0u;
  }
  return 1u;
}

/* ======================= I2C 回调分发 (由 app.c 调用, 中断上下文) ======================= */

void INA228_I2C_TxCplt(void)
{
  if (tx_is_read != 0u)
  {
    /* 寄存器地址已发出, 紧接着收数据(同一事务内 busy 保持, 不插队) */
    tx_is_read = 0u;
    if (HAL_I2C_Master_Receive_IT(&hi2c1, (uint16_t)(INA228_I2C_ADDR << 1),
                                  rxbuf, rx_len) != HAL_OK)
    {
      io_result = 2u;
      comm_ok   = 0u;
      I2C_Release();
    }
  }
  else
  {
    /* 普通写(如 SHUNT_CAL)完成, 释放总线 */
    io_result = 1u;
    I2C_Release();
  }
}

void INA228_I2C_RxCplt(void)
{
  data_pending = 1u;
  io_result    = 1u;
  I2C_Release();
}

void INA228_I2C_Error(void)
{
  tx_is_read   = 0u;
  data_pending = 0u;
  io_result    = 2u;
  comm_ok      = 0u;
  I2C_Release();
}

/* ======================= 对外接口 ======================= */

void INA228_Init(void)
{
  uint8_t i;

  comm_ok      = 1u;                    /* 先假设正常, 自检失败再清零 */
  cal_pending  = 0u;
  data_pending = 0u;
  tx_is_read   = 0u;
  io_result    = 0u;
  i_offset     = 0.0f;
  filt_v       = 0.0f;
  filt_i_raw   = 0.0f;
  filt_p       = 0.0f;
  dev_id       = 0u;
  vsum = 0.0f; isum = 0.0f; psum = 0.0f;
  vcount = 0u; icount = 0u; pcount = 0u;
  vidx = 0u;   iidx = 0u;   pidx = 0u;
  for (i = 0u; i < INA228_FILTER_SAMPLES; i++)
  {
    vbuf[i] = 0.0f;
    ibuf[i] = 0.0f;
    pbuf[i] = 0.0f;
  }

  /* ---- 1) 写 SHUNT_CAL(上电必须先写, 不写电流/功率恒为 0) ---- */
  txbuf[0] = INA228_REG_SHUNT_CAL;
  txbuf[1] = (uint8_t)(INA228_SHUNT_CAL >> 8);
  txbuf[2] = (uint8_t)(INA228_SHUNT_CAL & 0xFFu);
  if (I2C_TryAcquire(I2C_OWNER_INA228) != 0u)
  {
    tx_is_read = 0u;
    io_result  = 0u;
    if (HAL_I2C_Master_Transmit_IT(&hi2c1, (uint16_t)(INA228_I2C_ADDR << 1),
                                   txbuf, 3u) == HAL_OK)
    {
      I2C_WaitDone(I2C_OP_TIMEOUT_MS);
    }
    else
    {
      I2C_Release();
      io_result = 2u;
    }
    if (io_result != 1u)
    {
      comm_ok = 0u;
    }
  }
  else
  {
    comm_ok = 0u;
  }

  /* ---- 2) 读 DEVICE_ID 自检(16 位寄存器, 只读 2 字节) ---- */
  if (INA228_KickRead(INA228_REG_DEVICE_ID, 2u) != 0u)
  {
    I2C_WaitDone(I2C_OP_TIMEOUT_MS);
    if ((io_result == 1u) && (data_pending != 0u))
    {
      data_pending = 0u;
      INA228_Decode(INA228_REG_DEVICE_ID, rxbuf);
    }
    else
    {
      comm_ok = 0u;
    }
  }
  else
  {
    comm_ok = 0u;
  }

  /* ---- 3) 复位连读状态, 下一轮立刻开始 ---- */
  burst_active      = 0u;
  burst_idx         = 0u;
  next_set_us       = INA228_UsNow();
  burst_deadline_us = next_set_us;
}

void INA228_Task(void)
{
  uint8_t just_finished = 0u;

  /* 1) 有数据先解码(主循环里, 可以算浮点) */
  if (data_pending != 0u)
  {
    data_pending = 0u;
    INA228_Decode(rx_reg, rxbuf);
    just_finished = 1u;
  }

  /* 2) 连读调度: 每轮依次读 VBUS/CURRENT/POWER, 上一笔完成就接下一笔;
        抢不到总线就下个循环再试, 不阻塞 */
  if (burst_active != 0u)
  {
    if (just_finished != 0u)
    {
      if (burst_idx < 3u)
      {
        if (INA228_KickRead(burst_regs[burst_idx], 3u) != 0u)
        {
          burst_idx++;
          burst_deadline_us = INA228_UsNow() + INA228_BURST_TIMEOUT_US;
        }
      }
      if (burst_idx >= 3u)
      {
        burst_active = 0u;
        next_set_us  = INA228_UsNow() + INA228_SET_PERIOD_US;
      }
    }
    else if ((int32_t)(INA228_UsNow() - burst_deadline_us) >= 0)
    {
      /* 这一笔迟迟不回来(I2C 出错时回调会清掉 data_pending, 或者一直抢不到
         总线): 作废这一轮, 立刻重新开始。没有这一步的话 burst_active 会
         永远停在 1, 采样从此彻底停掉, 只能复位 */
      burst_active = 0u;
      next_set_us  = INA228_UsNow();
    }
  }
  else
  {
    if ((int32_t)(INA228_UsNow() - next_set_us) >= 0)
    {
      if (INA228_KickRead(burst_regs[0], 3u) != 0u)
      {
        burst_active      = 1u;
        burst_idx         = 1u;
        burst_deadline_us = INA228_UsNow() + INA228_BURST_TIMEOUT_US;
      }
      else
      {
        /* 总线被 OLED 占着, 50µs 后再试 */
        next_set_us = INA228_UsNow() + 50u;
      }
    }
  }
}

void INA228_CalibrateZero(void)
{
  uint8_t i;

  /* 偏移 = 当前滤波电流原始值(缓冲里存的是未扣偏移的值) */
  if (icount > 0u)
  {
    i_offset = filt_i_raw;
  }
  else
  {
    i_offset = 0.0f;
  }
  /* 缓冲要跟着一起清: 只清 isum 不清 ibuf 的话, 头 8 个新样本会被上次的
     旧值抵掉, 读数会先跳到 -offset 再慢慢爬回来 */
  for (i = 0u; i < INA228_FILTER_SAMPLES; i++)
  {
    ibuf[i] = 0.0f;
  }
  isum = 0.0f; icount = 0u; iidx = 0u;
  filt_i_raw = 0.0f;
  cal_pending = 1u;                     /* 完成标志, 等 app 取走 */
}

uint8_t INA228_IsCalPending(void)
{
  return cal_pending;
}

void INA228_ClearCalPending(void)
{
  cal_pending = 0u;
}

uint8_t INA228_CommOk(void)
{
  return comm_ok;
}

uint16_t INA228_GetDeviceId(void)
{
  return dev_id;
}

uint8_t INA228_IsDataReady(void)
{
  return data_pending;
}

/* µs 时钟: TIM1 自由运行计数, 48MHz/48 = 1MHz。
   C011 的 CMSIS 没有 DWT, 所以直接配 TIM1 寄存器, 第一次调用时初始化。
   TIM1 只有 16 位, 65.5ms 回绕一次, 这里用 uint16 差值累加成 32 位;
   外面比较时间必须用这个 32 位值, 直接拿 CNT 做差回绕一次就永远失效了 */
uint32_t INA228_UsNow(void)
{
  static uint8_t  ready    = 0u;
  static uint16_t last_cnt = 0u;
  static uint32_t accum    = 0u;
  uint16_t cnt;

  if (ready == 0u)
  {
    __HAL_RCC_TIM1_CLK_ENABLE();
    TIM1->PSC = 47u;              /* 48MHz / 48 = 1MHz */
    TIM1->ARR = 0xFFFFu;
    TIM1->EGR = TIM_EGR_UG;       /* 立刻加载预分频 */
    TIM1->CR1 = TIM_CR1_CEN;      /* 启动自由运行计数 */
    ready = 1u;
  }

  cnt   = (uint16_t)TIM1->CNT;
  accum += (uint32_t)(uint16_t)(cnt - last_cnt);   /* uint16 减法自己处理回绕 */
  last_cnt = cnt;
  return accum;
}

float INA228_GetVoltage_V(void)
{
  return filt_v;
}

float INA228_GetCurrent_A(void)
{
  /* 没样本时返回 0(校准清了缓冲后 icount==0, 也不会蹦出 -offset) */
  return (icount > 0u) ? (filt_i_raw - i_offset) : 0.0f;
}

float INA228_GetCurrentRaw_A(void)
{
  return (icount > 0u) ? filt_i_raw : 0.0f;
}

float INA228_GetOffset_A(void)
{
  return i_offset;
}

float INA228_GetPower_W(void)
{
  /* 功率取 POWER 寄存器的滤波值; 芯片是按"没扣零点偏移"的电流算的,
     所以这里要减掉 V x 偏移那一份, 不然空载时功率也不是 0 */
  if (pcount == 0u)
  {
    return 0.0f;
  }
  return filt_p - (filt_v * i_offset);
}
