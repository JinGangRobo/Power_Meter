#ifndef KK_OLED_DRIVER_H
#define KK_OLED_DRIVER_H

#include "kk_oled.h"

#include <stdbool.h>
#include <stdint.h>

/* KK_OLED 在本工程的硬件适配边界。
 * 本文件只声明契约，工程相关的硬件事实集中在 kk_oled_driver.c。 */

/** 本工程 SSD1306 模组的物理宽度。 */
#define OLED_PHYSICAL_WIDTH 128U
/** 本工程 SSD1306 模组的物理高度。 */
#define OLED_PHYSICAL_HEIGHT 64U
/** 每页 8 行像素，因此 64 行屏幕共有 8 页。 */
#define OLED_PHYSICAL_PAGES (OLED_PHYSICAL_HEIGHT / 8U)

/** 初始化 SSD1306：发送初始化序列、逐页清零显存后点亮显示。 */
OLED_Status OLED_DriverInit(void);

/** 阻塞发送核心准备好的全部差异页；成功返回时传输与提交均已完成。 */
OLED_Status OLED_DriverWriteBlocking(void);

/** 本工程 I2C1 的中断状态机由 INA228 的两步式读写占用，未实现异步刷新，固定返回 OLED_UNSUPPORTED。 */
OLED_Status OLED_DriverWriteIT(void);

/** 本工程未为 I2C1 配置 DMA 通道，固定返回 OLED_UNSUPPORTED。 */
OLED_Status OLED_DriverWriteDMA(void);

/** 查询驱动是否正在传输；阻塞刷新期间为 true。 */
bool OLED_DriverIsBusy(void);

/** 阻塞发送 SSD1306 对比度命令（0x81）。 */
OLED_Status OLED_DriverSetContrast(uint8_t value);

/** 阻塞发送显示关闭 0xAE 或显示开启 0xAF。 */
OLED_Status OLED_DriverSetPowerSave(bool enable);

/* 由应用 HAL 回调转发，不直接占用 HAL 全局弱回调。
 * 本工程未实现异步刷新，这两个入口是空实现，保留以满足 driver 契约。 */

/** 处理一次 I2C Memory Write 完成事件；当前无异步状态机，空实现。 */
void OLED_DriverHandleMemTxComplete(void);

/** 终止当前异步传输并向核心报告错误；当前无异步状态机，空实现。 */
void OLED_DriverHandleError(void);

#endif
