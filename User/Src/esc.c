/* esc.c —— 电调 PWM

   TIM3 配成 50Hz(20ms): 48MHz 分频到 1MHz, ARR=19999, 所以 CCR3 直接就是
   脉宽微秒数。开了预装载, 改脉宽在周期边界生效, 波形不会出毛刺。

   没启动时输出是关的, PA8 一直是低; 启动先给 1000µs(零速)再往上爬,
   每 10ms 爬 5µs; 过流或测不到电流时每 10ms 退 10µs, 退到底就关输出。
*/
#include "esc.h"
#include "tim.h"
#include "public_config.h"
#include "ina228.h"

static EscState_t esc_state = ESC_STATE_OFF;
static uint16_t   esc_pulse;         /* 当前脉宽(µs) */
static uint32_t   last_task_ms;

void ESC_Init(void)
{
  /* 重配 TIM3 为 50Hz(20ms): 1MHz tick, ARR = 19999 */
  htim3.Init.Prescaler = 47u;
  htim3.Init.Period    = 19999u;
  htim3.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim3.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_PWM_Init(&htim3) != HAL_OK)
  {
    Error_Handler();
  }

  /* 通道 3: PWM1, 没启动时 Pulse=0 */
  {
    TIM_OC_InitTypeDef oc = {0};
    oc.OCMode     = TIM_OCMODE_PWM1;
    oc.Pulse      = 0u;
    oc.OCPolarity = TIM_OCPOLARITY_HIGH;
    oc.OCFastMode = TIM_OCFAST_DISABLE;
    if (HAL_TIM_PWM_ConfigChannel(&htim3, &oc, TIM_CHANNEL_3) != HAL_OK)
    {
      Error_Handler();
    }
  }

  /* 预装载: CCR 在周期边界生效, 改脉宽不会有毛刺 */
  __HAL_TIM_ENABLE_OCxPRELOAD(&htim3, TIM_CHANNEL_3);

  esc_state    = ESC_STATE_OFF;
  esc_pulse    = 0u;
  last_task_ms = HAL_GetTick();
}

void ESC_Start(void)
{
  if (esc_state == ESC_STATE_OFF)
  {
    esc_pulse = ESC_PULSE_START_US;                /* 从 1000µs(零速)起步 */
    __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_3, esc_pulse);
    HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_3);
    esc_state = ESC_STATE_RAMP;
  }
  /* RAMP/HOLD 时再收到启动命令就忽略, 不重新爬一遍 */
}

void ESC_Stop(void)
{
  HAL_TIM_PWM_Stop(&htim3, TIM_CHANNEL_3);         /* 关输出, 电调收不到信号就停机 */
  esc_state = ESC_STATE_OFF;
  esc_pulse = 0u;
  __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_3, 0u);
}

void ESC_Task(void)
{
  uint32_t now = HAL_GetTick();

  if ((int32_t)(now - last_task_ms) < (int32_t)ESC_TASK_PERIOD_MS)
  {
    return;                                        /* 10ms 节流 */
  }
  last_task_ms = now;

  /* ---- 安全联动: 过流或 INA228 通信失败 -> FAULT ---- */
  if ((esc_state == ESC_STATE_RAMP) || (esc_state == ESC_STATE_HOLD))
  {
    float i = INA228_GetCurrent_A();
    if ((INA228_CommOk() == 0u)
        || (i > OVERCURRENT_THRESHOLD_A)
        || (i < -OVERCURRENT_THRESHOLD_A))
    {
      esc_state = ESC_STATE_FAULT;
    }
  }

  switch (esc_state)
  {
    case ESC_STATE_OFF:
      break;

    case ESC_STATE_RAMP:
      /* 线性爬升: 每 10ms +5µs */
      if (esc_pulse < ESC_PULSE_MAX_US)
      {
        esc_pulse += ESC_RAMP_STEP_US;
        if (esc_pulse > ESC_PULSE_MAX_US)
        {
          esc_pulse = ESC_PULSE_MAX_US;
        }
        __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_3, esc_pulse);
      }
      else
      {
        esc_state = ESC_STATE_HOLD;                /* 到顶保持 */
      }
      break;

    case ESC_STATE_HOLD:
      break;

    case ESC_STATE_FAULT:
      /* 2 倍步长退回起点, 然后彻底关输出 */
      if (esc_pulse > ESC_PULSE_START_US)
      {
        uint16_t step = ESC_RAMP_STEP_US * 2u;
        esc_pulse = ((uint32_t)esc_pulse > (uint32_t)step + ESC_PULSE_START_US)
                  ? (uint16_t)(esc_pulse - step) : ESC_PULSE_START_US;
        __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_3, esc_pulse);
      }
      else
      {
        ESC_Stop();
      }
      break;

    default:
      esc_state = ESC_STATE_OFF;
      break;
  }
}

EscState_t ESC_GetState(void)
{
  return esc_state;
}
