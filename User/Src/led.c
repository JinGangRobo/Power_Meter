/* led.c —— 两个灯的显示逻辑

   引脚只在 LED_Task() 里写(10ms 一次), 别处只调下面的 Set 函数给状态,
   免得几处代码同时改同一个引脚 */
#include "led.h"
#include "public_config.h"
#include "gpio.h"

/* ======================= 内部状态 ======================= */

typedef enum
{
  LED1_MODE_BLINK = 0,  /* 1Hz 慢闪 */
  LED1_MODE_ON,         /* 常亮 */
  LED1_MODE_ACK         /* 应答单闪 */
} Led1Mode_t;

static Led1Mode_t led1_mode;      /* 当前模式 */
static Led1Mode_t led1_prev;      /* ACK 之前的模式, ACK 完恢复回去 */
static uint32_t   led1_ack_start; /* ACK 起始时刻 */

static volatile uint8_t led2_comm_ok;   /* 通信正常 */
static volatile uint8_t led2_overcurr;  /* 过流 */
static volatile uint8_t led2_overvolt;  /* 过压 */

static uint32_t last_task_ms;

/* ======================= 内部函数 ======================= */

/* 写一个 LED: on=1 点亮(电平方向看 LED_ACTIVE_HIGH) */
static void Led_Write(uint16_t pin, uint8_t on)
{
  GPIO_PinState st;
#if (LED_ACTIVE_HIGH == 1)
  st = (on != 0u) ? GPIO_PIN_SET : GPIO_PIN_RESET;
#else
  st = (on != 0u) ? GPIO_PIN_RESET : GPIO_PIN_SET;
#endif
  HAL_GPIO_WritePin(LED_PORT, pin, st);
}

/* ======================= 对外接口 ======================= */

void LED_Init(void)
{
  led1_mode      = LED1_MODE_BLINK;
  led1_prev      = LED1_MODE_BLINK;
  led1_ack_start = 0u;
  led2_comm_ok   = 1u;
  led2_overcurr  = 0u;
  led2_overvolt  = 0u;
  last_task_ms   = HAL_GetTick();
}

void LED_Task(void)
{
  uint32_t now = HAL_GetTick();
  uint8_t on;

  if ((int32_t)(now - last_task_ms) < (int32_t)LED_TASK_PERIOD_MS)
  {
    return;                             /* 10ms 节流 */
  }
  last_task_ms = now;

  /* ---- LED1 ---- */
  switch (led1_mode)
  {
    case LED1_MODE_ACK:
      on = 1u;                          /* ACK: 点亮 200ms */
      if ((int32_t)(now - led1_ack_start) >= (int32_t)LED_ACK_MS)
      {
        led1_mode = led1_prev;          /* 时间到, 恢复之前的模式 */
      }
      break;

    case LED1_MODE_ON:
      on = 1u;
      break;

    case LED1_MODE_BLINK:
    default:
      on = ((now % 1000u) < 500u) ? 1u : 0u;   /* 1Hz 慢闪 */
      break;
  }
  Led_Write(LED1_PIN, on);

  /* ---- LED2 (按优先级从高到低) ---- */
  if (led2_comm_ok == 0u)
  {
    on = ((now % LED2_FAST_BLINK_PERIOD_MS)
          < (LED2_FAST_BLINK_PERIOD_MS / 2u)) ? 1u : 0u;   /* 通信失败: 4Hz 快闪 */
  }
  else if (led2_overcurr != 0u)
  {
    on = 1u;                            /* 过流: 常亮 */
  }
  else if (led2_overvolt != 0u)
  {
    on = ((now % LED2_SLOW_BLINK_PERIOD_MS)
          < (LED2_SLOW_BLINK_PERIOD_MS / 2u)) ? 1u : 0u;   /* 过压: 1Hz 慢闪 */
  }
  else
  {
    on = 0u;                            /* 全正常: 熄灭 */
  }
  Led_Write(LED2_PIN, on);
}

void LED1_SetReportActive(uint8_t active)
{
  Led1Mode_t target = (active != 0u) ? LED1_MODE_BLINK : LED1_MODE_ON;
  if (led1_mode == LED1_MODE_ACK)
  {
    led1_prev = target;                 /* ACK 进行中: 只更新恢复目标 */
  }
  else
  {
    led1_mode = target;
  }
}

void LED1_TriggerAck(void)
{
  if (led1_mode != LED1_MODE_ACK)
  {
    led1_prev      = led1_mode;         /* 记住之前的模式 */
    led1_mode      = LED1_MODE_ACK;
    led1_ack_start = HAL_GetTick();
  }
  /* 已在 ACK 中: 忽略重复触发 */
}

void LED2_SetCommOk(uint8_t ok)
{
  led2_comm_ok = ok;
}

void LED2_SetOvercurrent(uint8_t oc)
{
  led2_overcurr = oc;
}

void LED2_SetOvervoltage(uint8_t ov)
{
  led2_overvolt = ov;
}
