/* app.c —— 主程序逻辑都在这里

   主循环每圈依次跑:
     INA228_Task  读芯片(中断配合)
     OLED_Task    空函数(现在直接写屏, 不需要任务)
     Key_Task     按键
     LED_Task     两个灯
     ESC_Task     电调 PWM 爬升/回退
     串口命令      S 启动 / X 停止 / C 重校准
     App_Periodic 峰值、上传、刷屏(按各自周期)

   几点说明:
     - I2C 上挂了 INA228 和 OLED, 用 i2c_busy 抢总线, INA228 优先;
     - 功率直接读芯片算好的 POWER 寄存器, 固件不乘;
     - 串口拼字符串不用 printf/sprintf;
     - USART1 发二进制帧给电脑程序, USART2 发文本给人看。
*/
#include "app.h"
#include "i2c.h"
#include "usart.h"
#include "ina228.h"
#include "kk_oled.h"      /* KK_OLED 图形库 */
#include "kk_pm_font.h"   /* 本工程字模 */
#include "kk_pm_ui.h"     /* KK_UI 界面 */
#include "key.h"
#include "led.h"
#include "esc.h"
#include <string.h>

/* ======================= I2C 总线仲裁 ======================= */

volatile uint8_t i2c_busy  = 0u;
volatile uint8_t i2c_owner = (uint8_t)I2C_OWNER_NONE;

uint8_t I2C_TryAcquire(I2C_Owner_t owner)
{
  if (i2c_busy != 0u)
  {
    return 0u;
  }
  i2c_owner = (uint8_t)owner;
  i2c_busy  = 1u;
  return 1u;
}

void I2C_Release(void)
{
  i2c_busy  = 0u;
  i2c_owner = (uint8_t)I2C_OWNER_NONE;
}

void I2C_WaitDone(uint32_t timeout_ms)
{
  uint32_t t0 = HAL_GetTick();
  while (i2c_busy != 0u)
  {
    if ((int32_t)(HAL_GetTick() - t0) >= (int32_t)timeout_ms)
    {
      /* 超时就强制清掉, 免得总线上没接设备时开机卡死 */
      i2c_busy  = 0u;
      i2c_owner = (uint8_t)I2C_OWNER_NONE;
      break;
    }
  }
}

/* ======================= HAL I2C 回调 ======================= */

/* 这三个覆盖 HAL 的弱函数。中断里只看现在是谁在用总线,
   把"发完了/收完了/出错了"转给对应驱动 */

void HAL_I2C_MasterTxCpltCallback(I2C_HandleTypeDef *hi2c)
{
  if (hi2c->Instance != I2C1)
  {
    return;
  }
  /* 现在只有 INA228 用中断收发, OLED 是阻塞的 */
  if ((I2C_Owner_t)i2c_owner == I2C_OWNER_INA228)
  {
    INA228_I2C_TxCplt();
  }
}

void HAL_I2C_MasterRxCpltCallback(I2C_HandleTypeDef *hi2c)
{
  if (hi2c->Instance != I2C1)
  {
    return;
  }
  if ((I2C_Owner_t)i2c_owner == I2C_OWNER_INA228)
  {
    INA228_I2C_RxCplt();
  }
}

void HAL_I2C_ErrorCallback(I2C_HandleTypeDef *hi2c)
{
  if (hi2c->Instance != I2C1)
  {
    return;
  }
  if ((I2C_Owner_t)i2c_owner == I2C_OWNER_INA228)
  {
    INA228_I2C_Error();
  }
}

/* ======================= 串口发送(不用 printf) ======================= */

/* 不能用 printf/sprintf: 浮点格式化会进 HardFault。下面几个小工具
   自己拼字符串, 拼好了用 HAL_UART_Transmit 发 */

/* 发字符串到 USART2。发之前复位一下 UART 状态,
   万一上次卡在 BUSY 不放, 后面就一条也发不出去了 */
static void uart_puts(const char *s)
{
  uint16_t len = 0u;
  while (s[len] != '\0')
  {
    len++;
  }
  if (len == 0u)
  {
    return;
  }
  if (huart2.gState != HAL_UART_STATE_READY)
  {
    huart2.gState  = HAL_UART_STATE_READY;
    huart2.RxState = HAL_UART_STATE_READY;
  }
  (void)HAL_UART_Transmit(&huart2, (uint8_t *)s, len, 1000u);
}

/* 无符号整数转十进制字符串 */
static void uint_dec(char *buf, uint32_t v)
{
  char tmp[11];
  uint8_t i = 0u, j = 0u;
  if (v == 0u)
  {
    tmp[i++] = '0';
  }
  while (v > 0u)
  {
    tmp[i++] = (char)('0' + (v % 10u));
    v /= 10u;
  }
  while (i > 0u)
  {
    buf[j++] = tmp[--i];
  }
  buf[j] = '\0';
}

/* 无符号整数转十六进制字符串(大写, 不加前导零) */
static void uint_hex(char *buf, uint32_t v)
{
  static const char hexd[] = "0123456789ABCDEF";
  char tmp[9];
  uint8_t i = 0u, j = 0u;
  if (v == 0u)
  {
    tmp[i++] = '0';
  }
  while (v > 0u)
  {
    tmp[i++] = hexd[v & 0xFu];
    v >>= 4u;
  }
  while (i > 0u)
  {
    buf[j++] = tmp[--i];
  }
  buf[j] = '\0';
}

/* ============ 给电脑程序的二进制帧 (USART1, 115200) ============ */

/* 一帧 12 字节, 每 UPLOAD_FULL_PERIOD_US 发一次:
     [0..1] AA 55      帧头
     [2]    46         'F' 表示全量
     [3:4]  电压 mV    uint16 小端
     [5:6]  电流 mA    int16  小端, 有符号
     [7:8]  功率 0.01W uint16 小端
     [9:10] 峰值 0.01W uint16 小端
     [11]   前 11 字节求和取低 8 位 (给接收端校验用)
*/
static void Bin_Send(float v, float i, float p, float pk)
{
  uint8_t  f[12];
  int32_t  t;
  uint16_t u;
  uint8_t  k, sum;

  f[0] = 0xAAu;
  f[1] = 0x55u;
  f[2] = 0x46u;

  t = (int32_t)(v * 1000.0f + 0.5f);                /* 电压 -> mV */
  if (t < 0)     { t = 0; }
  if (t > 65535) { t = 65535; }
  u = (uint16_t)t;
  f[3] = (uint8_t)(u & 0xFFu);                      /* 小端: 低字节在前 */
  f[4] = (uint8_t)(u >> 8);

  t = (int32_t)(i * 1000.0f);                       /* 电流 -> mA, 有正负 */
  if (t > 32767)   { t = 32767; }
  if (t < -32768)  { t = -32768; }
  u = (uint16_t)(int16_t)t;
  f[5] = (uint8_t)(u & 0xFFu);
  f[6] = (uint8_t)(u >> 8);

  t = (int32_t)(p * 100.0f + 0.5f);                 /* 功率 -> 0.01W */
  if (t < 0)     { t = 0; }
  if (t > 65535) { t = 65535; }
  u = (uint16_t)t;
  f[7] = (uint8_t)(u & 0xFFu);
  f[8] = (uint8_t)(u >> 8);

  t = (int32_t)(pk * 100.0f + 0.5f);                /* 峰值 -> 0.01W */
  if (t < 0)     { t = 0; }
  if (t > 65535) { t = 65535; }
  u = (uint16_t)t;
  f[9]  = (uint8_t)(u & 0xFFu);
  f[10] = (uint8_t)(u >> 8);

  sum = 0u;
  for (k = 0u; k < 11u; k++)
  {
    sum += f[k];
  }
  f[11] = sum;

  if (huart1.gState != HAL_UART_STATE_READY)
  {
    huart1.gState  = HAL_UART_STATE_READY;
    huart1.RxState = HAL_UART_STATE_READY;
  }
  (void)HAL_UART_Transmit(&huart1, f, 12u, 50u);
}

/* 浮点转字符串, 比如 12.345。decimals 是小数位数。
   不能用 %f, 就乘 scale 取整再拼小数点 */
void App_FmtFloat(char *buf, float v, uint8_t decimals)
{
  int32_t  scale = (decimals == 4u) ? 10000
                 : (decimals == 3u) ? 1000
                 : (decimals == 2u) ? 100
                 : (decimals == 1u) ? 10 : 1;
  int32_t  n, ip;
  uint32_t fp;
  char     tmp[12];
  uint8_t  i, j;

  if (v < 0.0f)
  {
    *buf++ = '-';
    v = -v;
  }
  n  = (int32_t)(v * (float)scale + 0.5f);   /* 四舍五入到目标精度 */
  ip = n / scale;                            /* 整数部分 */
  fp = (uint32_t)(n % scale);                /* 小数部分 */

  /* 整数部分拼进缓冲 */
  i = 0u;
  if (ip == 0)
  {
    tmp[i++] = '0';
  }
  else
  {
    while (ip > 0)
    {
      tmp[i++] = (char)('0' + (ip % 10));
      ip /= 10;
    }
  }
  while (i > 0u)
  {
    *buf++ = tmp[--i];
  }

  /* 小数部分(不足位补 0) */
  if (decimals > 0u)
  {
    *buf++ = '.';
    for (j = decimals; j > 0u; j--)
    {
      tmp[j - 1u] = (char)('0' + (int)(fp % 10u));
      fp /= 10u;
    }
    for (j = 0u; j < decimals; j++)
    {
      *buf++ = tmp[j];
    }
  }
  *buf = '\0';
}

/* ======================= 峰值 / 显示 / 上传 ======================= */

static uint8_t      peak_valid  = 0u;           /* 功率峰值有数据没有 */
static float        peak_p_w    = 0.0f;         /* 最大功率(屏幕第 4 行 PK) */
static uint8_t      peak_v_valid = 0u;
static float        peak_v_v    = 0.0f;         /* 最大电压 */
static uint8_t      peak_i_valid = 0u;
static float        peak_i_a    = 0.0f;         /* 最大电流 */

/* ---- 屏幕数据 ----
   整屏绘制归 KK_UI, app 只提供数据: 瞬时值、峰值、一条临时提示 */
#define PM_MSG_COLS   16u       /* 临时提示最多 16 个字符 */

static char     msg[PM_MSG_COLS + 1u];    /* 实时页底行的临时提示文字 */
static uint32_t msg_until;                /* 提示显示到什么时候 */

static uint32_t last_stat_ms;             /* 上次更新峰值/保护灯 */
static uint32_t last_upload_us;           /* 上次发二进制帧 */
static uint32_t last_text_ms;             /* 上次发文本行 */

static uint32_t boot_cal_at;              /* 该做上电校准的时刻 */
static uint8_t  boot_cal_done;
static uint8_t  oled_ok;                  /* OLED 初始化是否成功 */

/* 显示一条临时提示(比如 CAL OK), MSG_SHOW_MS 后自动换回峰值 */
void App_SetMessage(const char *m)
{
  uint8_t i;
  for (i = 0u; i < PM_MSG_COLS; i++)
  {
    msg[i] = ' ';
  }
  msg[PM_MSG_COLS] = '\0';
  for (i = 0u; (i < PM_MSG_COLS) && (m[i] != '\0'); i++)
  {
    msg[i] = m[i];
  }
  msg_until = HAL_GetTick() + MSG_SHOW_MS;
}

/* ======================= 供 KK_UI 界面调用的接口 ======================= */

/* 峰值, 在 App_Periodic 里按 100ms 更新 */
float App_GetPeakPower_W(void)
{
  return peak_p_w;
}

float App_GetPeakVoltage_V(void)
{
  return peak_v_v;
}

float App_GetPeakCurrent_A(void)
{
  return peak_i_a;
}

/* 正在显示的临时提示(如 "CAL OK"); 没有提示时返回 NULL, 界面改显示峰值 */
const char *App_GetMessage(void)
{
  return ((int32_t)(HAL_GetTick() - msg_until) < 0) ? msg : 0;
}

/* 峰值清零: 从当前值重新开始记录。
   原来挂在按键短按上, 现在按键交给界面了, 改成控制菜单里的一项 */
void App_ClearPeaks(void)
{
  peak_valid   = 1u;
  peak_p_w     = INA228_GetPower_W();
  peak_v_valid = 1u;
  peak_v_v     = INA228_GetVoltage_V();
  peak_i_valid = 1u;
  peak_i_a     = INA228_GetCurrent_A();
  LED1_TriggerAck();
  uart_puts("PEAK CLEARED\r\n");
}

/* ======================= 电调 PWM 输出控制 ======================= */

/* 1 = 电调 PWM 正在输出 */
uint8_t App_GetEscOn(void)
{
  return (ESC_GetState() == ESC_STATE_OFF) ? 0u : 1u;
}

/* 启停电调 PWM: 界面和串口命令走同一条路径, 串口回执保持一致 */
void App_SetEsc(uint8_t on)
{
  if (on != 0u)
  {
    if (ESC_GetState() == ESC_STATE_OFF)
    {
      ESC_Start();
      App_SetMessage("ESC ON");
      uart_puts("ESC ON (ramp 1000->1300us)\r\n");
    }
  }
  else
  {
    if (ESC_GetState() != ESC_STATE_OFF)
    {
      ESC_Stop();
      App_SetMessage("ESC OFF");
      uart_puts("ESC OFF\r\n");
    }
  }
}

/* 切换电调 PWM 输出: 当前是开就关, 是关就开 */
void App_ToggleEsc(void)
{
  App_SetEsc((uint8_t)((App_GetEscOn() != 0u) ? 0u : 1u));
}

/* 拼一行文本: V=..V I=..A P=..W PK=..W */
static void Build_TextLine(float v, float i, float p, float pk, char *line)
{
  char sb[16];

  strcpy(line, "V=");
  App_FmtFloat(sb, v, 3);
  strcat(line, sb);
  strcat(line, "V ");
  strcat(line, "I=");
  App_FmtFloat(sb, i, 3);
  strcat(line, sb);
  strcat(line, "A ");
  strcat(line, "P=");
  App_FmtFloat(sb, p, 3);
  strcat(line, sb);
  strcat(line, "W ");
  strcat(line, "PK=");
  App_FmtFloat(sb, pk, 3);
  strcat(line, sb);
  strcat(line, "W\r\n");
}

/* 定期要干的事: 更新峰值和保护灯(100ms) / 发二进制帧(500Hz) /
   发文本(100ms) / 刷屏(200ms) */
static void App_Periodic(void)
{
  uint32_t now = HAL_GetTick();
  float v, i, p, ai;

  /* ---- 每 100ms: 更新峰值 + 保护灯 ---- */
  if ((int32_t)(now - last_stat_ms) >= (int32_t)UPLOAD_TEXT_PERIOD_MS)
  {
    last_stat_ms = now;
    v = INA228_GetVoltage_V();
    i = INA228_GetCurrent_A();
    p = INA228_GetPower_W();

    /* 比记录的还大就更新 */
    if ((peak_valid == 0u) || (p > peak_p_w))
    {
      peak_p_w   = p;
      peak_valid = 1u;
    }
    if ((peak_v_valid == 0u) || (v > peak_v_v))
    {
      peak_v_v    = v;
      peak_v_valid = 1u;
    }
    /* 电流峰值记绝对值: 反灌(负电流)也算, 跟过流判断保持一致 */
    ai = (i < 0.0f) ? -i : i;
    if ((peak_i_valid == 0u) || (ai > peak_i_a))
    {
      peak_i_a    = ai;
      peak_i_valid = 1u;
    }

    /* 给 LED2 喂状态, 优先级在 led.c 里判 */
    LED2_SetCommOk(INA228_CommOk());
    LED2_SetOvercurrent(((i > OVERCURRENT_THRESHOLD_A)
                      || (i < -OVERCURRENT_THRESHOLD_A)) ? 1u : 0u);
    LED2_SetOvervoltage((v > OVERVOLTAGE_THRESHOLD_V) ? 1u : 0u);
  }

  /* ---- 上传是常开的, 取最新滤波值 ---- */
  v = INA228_GetVoltage_V();
  i = INA228_GetCurrent_A();
  p = INA228_GetPower_W();

  /* USART1: 二进制帧, 每 2ms 一帧(500Hz) */
  if ((int32_t)(INA228_UsNow() - last_upload_us) >= (int32_t)UPLOAD_FULL_PERIOD_US)
  {
    last_upload_us = INA228_UsNow();
    Bin_Send(v, i, p, peak_p_w);
  }

  /* USART2: 文本行, 每 100ms 一行 */
  if ((int32_t)(now - last_text_ms) >= (int32_t)UPLOAD_TEXT_PERIOD_MS)
  {
    char line[64];
    last_text_ms = now;
    Build_TextLine(v, i, p, peak_p_w, line);
    uart_puts(line);
  }

}

/* 处理一个下行命令字符, 两个串口共用:
     S = 电调启动, X = 电调停止, C = 重新零点校准 */
static void App_HandleCommand(uint8_t c)
{
  if ((c == 'S') || (c == 's'))
  {
    ESC_Start();
    App_SetMessage("ESC ON");
    uart_puts("ESC ON (ramp 1000->1300us)\r\n");
  }
  else if ((c == 'X') || (c == 'x'))
  {
    ESC_Stop();
    App_SetMessage("ESC OFF");
    uart_puts("ESC OFF\r\n");
  }
  else if ((c == 'C') || (c == 'c'))
  {
    if (INA228_CommOk() != 0u)
    {
      INA228_CalibrateZero();
      uart_puts("RE-CAL...\r\n");
    }
  }
}

/* ======================= 应用入口 ======================= */

void App_Init(void)
{
  LED_Init();
  Key_Init();
  INA228_Init();        /* 写 SHUNT_CAL, 读 DEVICE_ID 自检 */
  /* KK_OLED: 初始化 SSD1306, 发初始化序列 + 清屏 + 点亮 */
  oled_ok = (OLED_Init() == OLED_OK) ? 1u : 0u;
  ESC_Init();           /* 电调 PWM: 配好 50Hz, 先不输出 */

  /* 开机图片: 画一次, 停留 SPLASH_HOLD_MS */
  KK_PM_UI_ShowSplash(SPLASH_HOLD_MS);

  /* 上电直接进入 KK_UI 界面(整屏绘制都归它) */
  KK_PM_UI_Init(HAL_GetTick());

  LED2_SetCommOk(INA228_CommOk());      /* 自检没过 -> LED2 4Hz 快闪 */
  /* LED1 默认 1Hz 慢闪, 不用额外设 */

  msg[0]         = '\0';
  msg_until      = 0u;
  peak_valid     = 0u;
  peak_p_w       = 0.0f;
  peak_v_valid   = 0u;
  peak_v_v       = 0.0f;
  peak_i_valid   = 0u;
  peak_i_a       = 0.0f;
  last_stat_ms   = HAL_GetTick();
  last_upload_us = INA228_UsNow();
  last_text_ms   = HAL_GetTick();

  /* 上电 1s 后自动校准一次, 这时滤波里已经有样本了 */
  boot_cal_at   = HAL_GetTick() + 1000u;
  boot_cal_done = 0u;

  /* ---- 开机信息 ---- */
  {
    char sb[16];
    uart_puts("\r\n=== TEST_power_meter_3 boot ===\r\n");
    uart_puts("INA228 self-test: ");
    uart_puts((INA228_CommOk() != 0u) ? "OK\r\n" : "FAIL\r\n");
    uart_puts("INA228 ID=0x");
    uint_hex(sb, (uint32_t)INA228_GetDeviceId());
    uart_puts(sb);
    uart_puts((INA228_GetDeviceId() == INA228_DEVICE_ID_VALUE)
              ? " (standard 0x2281)\r\n" : " (unexpected, non-fatal)\r\n");
    uart_puts("SHUNT_CAL=");
    uint_dec(sb, (uint32_t)INA228_SHUNT_CAL);
    uart_puts(sb);
    uart_puts("\r\n");
    uart_puts("SHUNT_R=");
    App_FmtFloat(sb, INA228_SHUNT_R_OHM, 4);
    uart_puts(sb);
    uart_puts("ohm\r\n");
    uart_puts("OLED addr=0x");
    uint_hex(sb, (uint32_t)OLED_I2C_ADDR);
    uart_puts(sb);
    uart_puts(" INA228 addr=0x");
    uint_hex(sb, (uint32_t)INA228_I2C_ADDR);
    uart_puts(sb);
    uart_puts(" OLED init=");
    uart_puts((oled_ok != 0u) ? "OK\r\n" : "FAIL\r\n");
    uart_puts("Display: KK_OLED 128x64 SSD1306, blocking refresh\r\n");
    uart_puts("KK_UI: ");
    uart_puts((KK_PM_UI_IsActive() != 0u)
              ? "ON (KEY1: short=next long=enter, KEY2: back)\r\n"
              : "INIT FAIL\r\n");
    uart_puts("USART1=bin 115200 (12B frame @500Hz), USART2=text 115200 10Hz\r\n");
    uart_puts("ESC PWM 50Hz: send 'S'=start(1000->1300us ramp) 'X'=stop 'C'=recal\r\n");
  }
}

void App_Task(void)
{
  /* 主循环: INA228 排在显示之前, 这样采样优先拿总线 */
  INA228_Task();
  Key_Task();
  LED_Task();
  ESC_Task();

  /* ---- 串口命令, 两个口都收:
   *      S = 电调启动, X = 停止, C = 重新零点校准 ---- */
  {
    uint8_t c;
    if (HAL_UART_Receive(&huart1, &c, 1u, 0u) == HAL_OK)
    {
      App_HandleCommand(c);
    }
    if (HAL_UART_Receive(&huart2, &c, 1u, 0u) == HAL_OK)
    {
      App_HandleCommand(c);
    }
  }

  /* ---- KK_UI 消费按键并绘制界面 ---- */
  KK_PM_UI_Update(HAL_GetTick());
  KK_PM_UI_ProcessEvents();

  /* ---- 上电自动零点校准, 只做一次 ---- */
  if ((boot_cal_done == 0u) && ((int32_t)(HAL_GetTick() - boot_cal_at) >= 0))
  {
    boot_cal_done = 1u;
    if (INA228_CommOk() != 0u)
    {
      INA228_CalibrateZero();
      uart_puts("AUTO ZERO CAL\r\n");
    }
  }

  /* ---- 取走校准完成标志, 不清的话 OLED 会一直停在 CAL OK ---- */
  if (INA228_IsCalPending() != 0u)
  {
    char sb[16];
    INA228_ClearCalPending();
    uart_puts("CAL OK offset=");
    App_FmtFloat(sb, INA228_GetOffset_A(), 4);
    uart_puts(sb);
    uart_puts("A\r\n");
    App_SetMessage("CAL OK");
  }

  App_Periodic();
}
