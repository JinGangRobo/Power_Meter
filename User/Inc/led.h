/* led.h —— 两个指示灯 (低电平点亮)

   LED1(PA4) 状态灯: 1Hz 慢闪=正常, 单闪 200ms=按键响了
   LED2(PA5) 故障灯, 按优先级显示:
        通信失败(4Hz 快闪) > 过流(常亮) > 过压(1Hz 慢闪) > 全正常(灭)

   引脚只在 LED_Task() 里写, 别的地方只调用下面这些 Set 函数给状态。
*/
#ifndef __LED_H
#define __LED_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

void LED_Init(void);
void LED_Task(void);                /* 主循环周期调用(内部 10ms 节流) */

/* LED1: 1 = 正常上报(1Hz 慢闪), 0 = 上报暂停(常亮) */
void LED1_SetReportActive(uint8_t active);
/* LED1: ACK 单闪 200ms, 结束后恢复之前模式 */
void LED1_TriggerAck(void);

/* LED2 状态输入 */
void LED2_SetCommOk(uint8_t ok);          /* 1 = 通信正常, 0 = 失败(4Hz 快闪) */
void LED2_SetOvercurrent(uint8_t oc);     /* 1 = 过流(常亮) */
void LED2_SetOvervoltage(uint8_t ov);     /* 1 = 过压(1Hz 慢闪) */

#ifdef __cplusplus
}
#endif

#endif /* __LED_H */
