/* key.c —— 两个按键

   KEY1(PA6): 上升沿触发(松手), 短按 = 下一个, 长按 = 确认进入
   KEY2(PA7): 只做电平扫描, 短按 = 返回

   中断里只置标志, 消抖和长短按都在主循环的状态机里做:
   IDLE -> 按下 -> 消抖 20ms -> 按住计时 -> 松手/到 1 秒出事件。

   KEY1 在 .ioc 里配的是上升沿, 也就是"松手"才进中断, 所以:
     - "按下"靠主循环读电平判断;
     - 上升沿标志用来更快地发现松手。
   回调必须写 HAL_GPIO_EXTI_Rising_Callback, 这个 HAL 的 EXTI 只认
   Rising/Falling 两个回调, HAL_GPIO_EXTI_Callback 不会被调用。

   KEY2 不占 EXTI: 它的上升沿也挂在同一个 EXTI4_15 中断上, 会和 KEY1 的
   标志互相干扰; 返回键只需要短按, 电平扫描足够。
*/
#include "key.h"
#include "public_config.h"
#include "gpio.h"

/* ======================= 内部状态 ======================= */

typedef enum
{
  KS_IDLE = 0,      /* 空闲, 等待按下 */
  KS_DEBOUNCE,      /* 消抖确认中 */
  KS_HOLD,          /* 按住中, 尚未到长按时间 */
  KS_LONG_DONE      /* 长按已触发, 等松手(期间不重复触发) */
} KeyState_t;

typedef struct
{
  KeyState_t state;
  uint32_t   press_ms;   /* 按下起始时刻 */
} KeyCtl_t;

/* 一个按键的静态描述: 引脚、短按/长按事件、是否用 EXTI 标志 */
typedef struct
{
  uint16_t pin;
  uint8_t  short_evt;
  uint8_t  long_evt;     /* 0 = 这个键不产生长按事件 */
  uint8_t  use_exti;
} KeyDesc_t;

/* 按键个数: 下面 key_desc 的项数必须与它一致 */
#define KEY_COUNT   2u
#define KEY_IDX1    0u     /* key_desc / key 里 KEY1(PA6) 的下标 */
#define KEY_IDX2    1u     /* key_desc / key 里 KEY2(PA7) 的下标 */

static const KeyDesc_t key_desc[KEY_COUNT] =
{
  {KEY1_PIN, (uint8_t)KEY1_SHORT, (uint8_t)KEY1_LONG, 1u},
  {KEY2_PIN, (uint8_t)KEY2_SHORT, 0u,                0u}
};

static volatile uint8_t exti_flag;              /* EXTI 中断只置标志(上升沿=松手) */
static KeyCtl_t key[KEY_COUNT];
static uint8_t  evt_queue[KEY_EVENT_QUEUE_SIZE];
static uint8_t  evt_head, evt_tail;

/* ======================= 内部函数 ======================= */

/* 读取按键电平: 按下(低电平)返回 1 */
static uint8_t Key_IsPressed(uint16_t pin)
{
  return (HAL_GPIO_ReadPin(KEY_PORT, pin) == GPIO_PIN_RESET) ? 1u : 0u;
}

/* 事件入队, 满就丢 */
static void Key_PushEvent(uint8_t evt)
{
  uint8_t next = (uint8_t)((evt_head + 1u) % KEY_EVENT_QUEUE_SIZE);
  if (next != evt_tail)
  {
    evt_queue[evt_head] = evt;
    evt_head = next;
  }
}

/* 单个按键的状态机, 每轮 Key_Task 执行一次 */
static void Key_ScanOne(uint8_t idx, uint32_t now, uint8_t edge)
{
  const KeyDesc_t *d = &key_desc[idx];
  KeyCtl_t        *k = &key[idx];
  uint8_t          pressed = Key_IsPressed(d->pin);

  switch (k->state)
  {
    case KS_IDLE:
      /* 按下看电平(上升沿是松手沿, 按下时不会有标志) */
      if (pressed != 0u)
      {
        k->state    = KS_DEBOUNCE;
        k->press_ms = now;
      }
      break;

    case KS_DEBOUNCE:
      if ((int32_t)(now - k->press_ms) >= (int32_t)KEY_DEBOUNCE_MS)
      {
        /* 电平稳定就确认按下, 消抖期内弹起判为抖动 */
        k->state = (pressed != 0u) ? KS_HOLD : KS_IDLE;
      }
      break;

    case KS_HOLD:
      if ((pressed == 0u) || ((d->use_exti != 0u) && (edge != 0u)))
      {
        k->state = KS_IDLE;                      /* 未到长按时间就松手 -> 短按 */
        if (d->short_evt != 0u)
        {
          Key_PushEvent(d->short_evt);
        }
      }
      else if ((d->long_evt != 0u)
               && ((int32_t)(now - k->press_ms) >= (int32_t)KEY_LONG_PRESS_MS))
      {
        k->state = KS_LONG_DONE;                 /* 到长按时间 -> 长按, 只触发一次 */
        Key_PushEvent(d->long_evt);
      }
      break;

    case KS_LONG_DONE:
      if ((pressed == 0u) || ((d->use_exti != 0u) && (edge != 0u)))
      {
        k->state = KS_IDLE;                      /* 松手后才允许下一次按下 */
      }
      break;

    default:
      k->state = KS_IDLE;
      break;
  }
}

/* ======================= HAL EXTI 回调 (中断里只置标志) ======================= */

/* 这个 HAL 实际调用的是下面这个弱回调, 不能用 HAL_GPIO_EXTI_Callback */
void HAL_GPIO_EXTI_Rising_Callback(uint16_t GPIO_Pin)
{
  if (GPIO_Pin == KEY1_PIN)
  {
    exti_flag = 1u;
  }
}

/* ======================= 对外接口 ======================= */

void Key_Init(void)
{
  uint8_t i;

  for (i = 0u; i < KEY_COUNT; i++)
  {
    key[i].state    = KS_IDLE;
    key[i].press_ms = 0u;
  }
  exti_flag = 0u;
  evt_head  = 0u;
  evt_tail  = 0u;

  /* 板上没外部上拉, 这里补内部上拉(CubeMX 里 PA6 是 NOPULL, PA7 没有配置)。
     只是重配输入模式, 不碰 AF */
#if (KEY_USE_INTERNAL_PULLUP == 1)
  {
    GPIO_InitTypeDef gpio = {0};

    /* KEY1: 保持上升沿中断 */
    gpio.Pin   = KEY1_PIN;
    gpio.Mode  = GPIO_MODE_IT_RISING;
    gpio.Pull  = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(KEY_PORT, &gpio);

    /* KEY2: 普通输入, 只做电平扫描 */
    gpio.Pin  = KEY2_PIN;
    gpio.Mode = GPIO_MODE_INPUT;
    HAL_GPIO_Init(KEY2_PORT, &gpio);
  }
#endif
}

void Key_Task(void)
{
  uint32_t now  = HAL_GetTick();
  uint8_t  edge = exti_flag;   /* 上升沿标志: 本周期内松过手 */
  exti_flag = 0u;              /* 标志用完清零 */

  Key_ScanOne(KEY_IDX1, now, edge);  /* KEY1 用 EXTI 标志更快发现松手 */
  Key_ScanOne(KEY_IDX2, now, 0u);    /* KEY2 只看电平 */
}

KeyEvent_t Key_GetEvent(void)
{
  KeyEvent_t evt;
  if (evt_head == evt_tail)
  {
    return KEY_EVT_NONE;
  }
  evt = (KeyEvent_t)evt_queue[evt_tail];
  evt_tail = (uint8_t)((evt_tail + 1u) % KEY_EVENT_QUEUE_SIZE);
  return evt;
}
