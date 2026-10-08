/* app.h —— 应用层入口 + I2C 总线仲裁接口

   INA228 和 OLED 挂在同一条 I2C 上, 一次只能一个说话, 所以用一套简单的
   "占坑"机制(i2c_busy + 当前是谁): 谁要发数据先抢, 抢不到就跳过这轮,
   用完释放。INA228 优先, OLED 有空隙才发。
*/
#ifndef __APP_H
#define __APP_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include "public_config.h"

/* ======================= I2C 总线 owner ======================= */
typedef enum
{
  I2C_OWNER_NONE = 0,   /* 总线空闲 */
  I2C_OWNER_INA228,     /* INA228 正在使用 */
  I2C_OWNER_OLED        /* OLED 正在使用 */
} I2C_Owner_t;

/* ======================= 应用入口 ======================= */
void App_Init(void);    /* 上电初始化(main.c 的 USER CODE 2 调用一次) */
void App_Task(void);    /* 应用主任务(main.c 主循环调用) */

/* ======================= 供 KK_UI 界面调用的接口 ======================= */
/* 只读数据: 峰值原先显示在 4 行 OLED 上, 现在由界面层读取 */
float       App_GetPeakPower_W(void);
float       App_GetPeakVoltage_V(void);
float       App_GetPeakCurrent_A(void);
/* 正在显示的临时提示(如 "CAL OK"), 无提示时返回 NULL */
const char *App_GetMessage(void);
/* 动作: 由界面层在菜单/页面上触发 */
/* 峰值清零: 从当前值重新记录(原按键短按功能) */
void        App_ClearPeaks(void);
/* 显示一条临时提示(如 "CAL OK"), MSG_SHOW_MS 后自动换回峰值 */
void        App_SetMessage(const char *m);
/* 电调 PWM: 1 = 正在输出。界面与串口命令共用这条路径 */
uint8_t     App_GetEscOn(void);
void        App_SetEsc(uint8_t on);
void        App_ToggleEsc(void);
/* 浮点转字符串(保留 decimals 位小数, 自动四舍五入)。
   不能用 printf/sprintf: 浮点格式化会进 HardFault。
   界面层与串口输出共用这一份实现 */
void        App_FmtFloat(char *buf, float v, uint8_t decimals);

/* ======================= I2C 总线仲裁 ======================= */

/* 尝试抢总线: 空闲则置 busy 并记录 owner, 返回 1; 忙则返回 0 */
uint8_t I2C_TryAcquire(I2C_Owner_t owner);
/* 释放总线(事务最后一步调用, 可在中断里调用) */
void     I2C_Release(void);
/* 自旋等当前事务结束, 超时就强制清标志(只在初始化时用, 免得没设备时卡死开机) */
void     I2C_WaitDone(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* __APP_H */
