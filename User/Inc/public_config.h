/* public_config.h —— 硬件参数都放在这个文件里, 要改参数只改这里 */

/* 引脚分配 (STM32C011F6P6):
     PA0/PA1   USART1   给电脑传数据(二进制帧, 115200)
     PA2/PA3   USART2   接电脑串口工具(文本, 115200)
     PA4/PA5   LED1/LED2   低电平点亮
     PA6       KEY1   界面用: 短按 = 下一个, 长按 = 确认, 上升沿触发(按下接地)
     PA7       KEY2   界面用: 短按 = 返回, 固件配普通输入 + 内部上拉
     PB6/PB7   I2C1    INA228 和 OLED 挂同一条总线
     PA8       TIM3_CH3  电调 PWM 输出
*/
#ifndef __PUBLIC_CONFIG_H
#define __PUBLIC_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

/* ======================= I2C 总线 ======================= */
/* 上电初始化时等待一次 I2C 事务完成的最长时间 */
#define I2C_OP_TIMEOUT_MS            50u

/* ======================= OLED (SSD1306) ======================= */
/* 7 位从机地址, SA0 接地是 0x3C */
#define OLED_I2C_ADDR                0x3Cu
/* 上电等屏稳定 */
#define OLED_POWERON_DELAY_MS        100u
/* 整屏刷新的最小间隔 */
#define OLED_REFRESH_PERIOD_MS       500u

/* ======================= INA228 ======================= */
/* 7 位从机地址: A0 接 GND 为 0x40, A0 接 VS 为 0x41 */
#define INA228_I2C_ADDR              0x40u
/* 采样电阻(欧姆), SHUNT_CAL 跟着它自动算 */
#define INA228_SHUNT_R_OHM           0.001f
/* 电流 LSB(安培), 1mA 分辨率 */
#define INA228_CURRENT_LSB_A         0.001f
/* SHUNT_CAL = 13107.2e6 x 电流LSB x 采样电阻(TI 手册公式),
 * 上电必须先写入, 不写电流和功率一直是 0 */
#define INA228_SHUNT_CAL             ((uint32_t)(13107.2e6 * (INA228_CURRENT_LSB_A) * (INA228_SHUNT_R_OHM) + 0.5))
/* VBUS 寄存器 LSB = 195.3125 uV */
#define INA228_VBUS_LSB_UV           195.3125
/* 每 600µs 连读 VBUS/CURRENT/POWER 一套 */
#define INA228_SET_PERIOD_US         600u
/* 连读超时: 一轮里任一寄存器超过这个时间没读回来就作废重来。
 * 一次读只要几百 µs, 这里留了很大余量; 没有它的话 I2C 出错后连读会停死 */
#define INA228_BURST_TIMEOUT_US      10000u
/* V/I/P 环形滤波缓冲深度 */
#define INA228_FILTER_SAMPLES        8u

/* ======================= 保护阈值 ======================= */
#define OVERCURRENT_THRESHOLD_A      25.0f   /* 过流阈值(取电流绝对值) */
#define OVERVOLTAGE_THRESHOLD_V      40.0f   /* 过压阈值 */

/* ======================= 按键 ======================= */
#define KEY_PORT                     GPIOA
#define KEY1_PIN                     GPIO_PIN_6
/* 板上没有外部上拉, 置 1 由固件补内部上拉 */
#define KEY_USE_INTERNAL_PULLUP      1u
/* 第二个按键: 返回。TSSOP20 第 14 脚 PA7, 紧挨着 PA6, 接线方便。
 * 同样没有外部上拉, 由固件补内部上拉; 只用电平扫描, 不占 EXTI。 */
#define KEY2_PORT                    GPIOA
#define KEY2_PIN                     GPIO_PIN_7
#define KEY_DEBOUNCE_MS              20u     /* 消抖时间 */
#define KEY_LONG_PRESS_MS            1000u   /* 长按判定时间 */
#define KEY_EVENT_QUEUE_SIZE         8u      /* 事件队列深度 */

/* ======================= LED ======================= */
#define LED_PORT                     GPIOA
#define LED1_PIN                     GPIO_PIN_4
#define LED2_PIN                     GPIO_PIN_5
/* LED 极性: 0 = 低电平点亮, 1 = 高电平点亮 */
#define LED_ACTIVE_HIGH              0u
/* LED 刷新周期 */
#define LED_TASK_PERIOD_MS           10u
/* LED1 应答单闪时长 */
#define LED_ACK_MS                   200u
/* LED2 通信失败快闪 4Hz */
#define LED2_FAST_BLINK_PERIOD_MS    250u
/* LED2 过压慢闪 1Hz */
#define LED2_SLOW_BLINK_PERIOD_MS    1000u

/* ======================= 电调 PWM (ESC) ======================= */
/* 50Hz 帧。启动第一帧给 1000µs(零速点), 每 ESC_TASK_PERIOD_MS 涨
 * ESC_RAMP_STEP_US, 到 1300µs 封顶(500µs/s, 全程 0.6s) */
#define ESC_PULSE_START_US           1000u
#define ESC_PULSE_MAX_US             1300u
#define ESC_RAMP_STEP_US             5u
#define ESC_TASK_PERIOD_MS           10u

/* ======================= 应用层 ======================= */
/* USART1 二进制帧周期: 2000µs = 500Hz, 12 字节/帧 */
#define UPLOAD_FULL_PERIOD_US        2000u
/* USART2 文本行周期 */
#define UPLOAD_TEXT_PERIOD_MS        100u
/* 瞬时提示(如 CAL OK)在实时页底行的停留时间 */
#define MSG_SHOW_MS                  2000u
/* 开机图片停留时间 */
#define SPLASH_HOLD_MS               1200u

#ifdef __cplusplus
}
#endif

#endif /* __PUBLIC_CONFIG_H */
