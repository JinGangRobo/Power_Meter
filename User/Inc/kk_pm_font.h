/* kk_pm_font.h —— 本工程用到的 KK_OLED 字模
 *
 * 字模由 LEDFont Agent API v1 按需生成，只包含界面实际会显示的字符，
 * 编译期不依赖在线服务。
 *
 * 生成参数（可在 LEDFont 的实时字体目录中核对）：
 *   font        wenquanyi_13px（文泉驿·点阵字14）
 *   size        14
 *   字符集      界面实际用到的 60 个字符，见下面的列表
 *   数组名      kk_font_pm
 *   实际字节    784
 *
 * 字符集（按界面实际会显示的字符逐字收集，不含未使用的字形）：
 *   " -./0123456789:=ABCDEFIKLMNOPRSTUVWXZabcdefghiklmnoprstuvxyz"
 * 早期版本生成的是可打印 ASCII 全集（95 字形 / 1192 字节），在 32KB Flash 上
 * 让首页图标选择器放不下，因此收缩为当前子集。
 *
 * 服务端未返回字体许可声明；该字模不因此转为 MIT 许可。
 * 界面文字、字号变化或新增字符后，需要用 kk-oled-font 重新检查并生成子集。
 */
#ifndef __KK_PM_FONT_H
#define __KK_PM_FONT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/** 14 号正文/数值字模，覆盖可打印 ASCII。传给 OLED_SetFont() 使用。 */
extern const uint8_t kk_font_pm[];

#ifdef __cplusplus
}
#endif

#endif /* __KK_PM_FONT_H */
