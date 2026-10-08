/* ina228.h —— 功率计芯片驱动

   用到的寄存器(别抄成 INA226 的, 地址不一样):
     SHUNT_CAL(0x02)  16 位, 上电必须先写, 不写电流和功率一直是 0
     VBUS(0x05)       24 位里高 20 位有效, 无符号, >>4 后 x 195.3125µV
     CURRENT(0x07)    24 位里高 20 位有效, 有符号, >>4 后 x 电流LSB
     POWER(0x08)      整个 24 位都是有符号数, 不用 >>4, x 3.2mW 就是瓦数
                      (芯片自己算好了 VxI, 固件不再乘)
     DEVICE_ID(0x3F)  正版读出来是 0x2281

   读一个寄存器分两步: 先发 1 字节寄存器地址, 再收 3 字节。
   全是中断方式(_IT), 谁用总线由 app.c 仲裁。
*/
#ifndef __INA228_H
#define __INA228_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include "public_config.h"

/* INA228 寄存器地址 */
#define INA228_REG_SHUNT_CAL        0x02u
#define INA228_REG_VBUS             0x05u
#define INA228_REG_CURRENT          0x07u
#define INA228_REG_POWER            0x08u
#define INA228_REG_DEVICE_ID        0x3Fu

/* DEVICE_ID 期望值 */
#define INA228_DEVICE_ID_VALUE      0x2281u

/* POWER 寄存器 LSB = 3.2 x 电流 LSB */
#define INA228_POWER_LSB_W          (3.2f * INA228_CURRENT_LSB_A)

/* ======================= 对外接口 ======================= */

/* 上电初始化: 写 SHUNT_CAL + 读 DEVICE_ID 自检。
 * 里面是 _IT + 自旋等待, 只能在初始化阶段调用, 主循环里别调 */
void     INA228_Init(void);

/* 主循环周期调用(放在 OLED_Task 之前, INA228 优先抢总线) */
void     INA228_Task(void);

/* 零点校准: 偏移 = 当前滤波电流原始值, 并置校准完成标志 */
void     INA228_CalibrateZero(void);

uint8_t  INA228_IsCalPending(void);   /* 校准完成标志(待 app 取走清零) */
void     INA228_ClearCalPending(void);
uint8_t  INA228_CommOk(void);         /* 通信状态: 地址有应答就算正常 */
uint16_t INA228_GetDeviceId(void);    /* 自检读回的 DEVICE_ID */
uint8_t  INA228_IsDataReady(void);    /* 有新采样待解码(调试用) */
uint32_t INA228_UsNow(void);          /* µs 时钟, 上传调度用 */

float    INA228_GetVoltage_V(void);   /* 滤波后电压 */
float    INA228_GetCurrent_A(void);   /* 滤波后电流(已扣除零点偏移) */
float    INA228_GetCurrentRaw_A(void);/* 滤波后电流原始值(未扣偏移) */
float    INA228_GetOffset_A(void);    /* 当前零点偏移 */
float    INA228_GetPower_W(void);     /* 滤波后功率: POWER 寄存器值减去 V x 零点偏移 */

/* ======================= I2C 回调分发入口 (由 app.c 的 HAL 回调调用) ======================= */
void     INA228_I2C_TxCplt(void);
void     INA228_I2C_RxCplt(void);
void     INA228_I2C_Error(void);

#ifdef __cplusplus
}
#endif

#endif /* __INA228_H */
