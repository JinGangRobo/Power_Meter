/* key.h —— 按键 (只有 PA6 一个键)

   中断里只置一个标志, 长短按判断放在主循环的状态机里做。

   短按(<1s 松手)  = 清峰值
   长按(按住满 1s) = 电调 PWM 启停, 只触发一次, 松手后才能再按
*/
#ifndef __KEY_H
#define __KEY_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

/* 事件码 */
typedef enum
{
  KEY_EVT_NONE = 0,     /* 无事件 */
  KEY1_SHORT,           /* KEY1(PA6) 短按: 界面里 = 下一个 */
  KEY1_LONG,            /* KEY1(PA6) 长按: 界面里 = 确认/进入 */
  KEY2_SHORT            /* KEY2(PA7) 短按: 返回 */
} KeyEvent_t;

void        Key_Init(void);
void        Key_Task(void);       /* 主循环周期调用 */
KeyEvent_t  Key_GetEvent(void);   /* 从队列取一个事件, 无事件返回 KEY_EVT_NONE */

#ifdef __cplusplus
}
#endif

#endif /* __KEY_H */
