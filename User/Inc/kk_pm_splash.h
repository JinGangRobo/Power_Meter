/* kk_pm_splash.h —— 开机图片
 *
 * 来源：工程根目录的 jgrobo.svg（只取其中的标志图形，去掉了 JGROBO 字样）。
 * 转换方式：无头 Chrome 按 SVG 实际图形边界（5.24,0 74.19x56.16）等比渲染成
 * 72×54 的 1bit 图，再裁到内容边界、按 XBM 位序（每字节低位在左、行优先）打包。
 *
 * 尺寸选择受 Flash 限制：本机 32KB。去掉未使用的 DMA 配置、并消掉重复的浮点
 * 格式化代码后，留给图片的空间约 600 字节，"画一次图片"的代码（OLED_DrawXBM 等）
 * 还要占 80 字节左右，因此图片取 72×54（486 字节），编完余约 270 字节。
 * 想放大到 80×60（600 字节）需要再腾出约 120 字节，整屏 128×64（1024 字节）
 * 需要再腾出约 550 字节。
 */
#ifndef __KK_PM_SPLASH_H
#define __KK_PM_SPLASH_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define KK_PM_SPLASH_W      72u   /* 图片宽(像素) */
#define KK_PM_SPLASH_H      54u   /* 图片高(像素) */
#define KK_PM_SPLASH_BYTES  486u  /* 字节数 = (W+7)/8 * H */
#define KK_PM_SPLASH_X      28    /* 画到屏上的 x, 128 屏居中 */
#define KK_PM_SPLASH_Y      5     /* 画到屏上的 y, 64 屏居中 */

extern const uint8_t kk_pm_splash[KK_PM_SPLASH_BYTES];

#ifdef __cplusplus
}
#endif

#endif /* __KK_PM_SPLASH_H */
