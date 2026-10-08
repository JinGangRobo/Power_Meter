/* esc.h —— 电调 PWM 控制

   PA8 输出 50Hz PWM, 脉宽就是转速指令。
   电调只认 1000~2000µs, 1000µs 是"零速", 所以启动时先给 1000µs,
   再按斜坡慢慢加到 1300µs, 停止直接关输出。
*/
#ifndef __ESC_H
#define __ESC_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

/* 状态 */
typedef enum
{
  ESC_STATE_OFF = 0,   /* 输出关闭 */
  ESC_STATE_RAMP,      /* 爬升中 */
  ESC_STATE_HOLD,      /* 到顶保持 */
  ESC_STATE_FAULT      /* 故障回退中(超流/通信失败) */
} EscState_t;

void       ESC_Init(void);          /* 上电初始化: 配置 50Hz, 不启动输出 */
void       ESC_Task(void);          /* 主循环周期调用(内部 10ms 节流) */
void       ESC_Start(void);         /* 启动: 从 1000µs 开始线性爬升 */
void       ESC_Stop(void);          /* 停止: 关闭输出 */
EscState_t ESC_GetState(void);      /* 当前状态 */

#ifdef __cplusplus
}
#endif

#endif /* __ESC_H */
