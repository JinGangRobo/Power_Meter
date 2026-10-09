/* kk_pm_ui.h —— 用 KK_UI 搭的功率计界面（应用层）
 *
 * 界面归 KK_UI 所有：进入菜单后由 KK_UI 负责导航、动画、整屏绘制和刷新调度，
 * 应用层只提供页面描述、业务变量和事件处理。
 *
 * 输入：本板有两个按键，在 kk_pm_ui.c 里归一化成 KK_UI 的三档操作
 *   KEY1(PA6) 短按 = DOWN（下一个条目）
 *   KEY1(PA6) 长按 = OK（进入 / 确认）
 *   KEY2(PA7) 短按 = 返回上一页
 * KK_UI 的普通菜单还会自动追加一个返回项，所以没接 KEY2 也能走完全部导航。
 * 接上三键或编码器后，只需要改 kk_pm_ui.c 里的输入归一化，页面描述不用动。
 */
#ifndef __KK_PM_UI_H
#define __KK_PM_UI_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/** 初始化 KK_UI 与本工程的页面描述；成功后才接管屏幕。 */
void KK_PM_UI_Init(uint32_t now_ms);

/** 开机图片：整屏画一次 kk_pm_splash 并停留 hold_ms。
 *  必须在 OLED_Init() 之后、KK_PM_UI_Init() 之前调用。 */
void KK_PM_UI_ShowSplash(uint32_t hold_ms);

/** 主循环周期调用：取按键事件、归一化成 KK_UI 输入、驱动一帧。 */
void KK_PM_UI_Update(uint32_t now_ms);

/** 取走 KK_UI 的业务事件并执行（峰值清零、零点校准、PWM 输出启停）。 */
void KK_PM_UI_ProcessEvents(void);

/** 1 = KK_UI 已经接管屏幕与按键；0 = 初始化失败，屏幕没有别的显示路径。 */
uint8_t KK_PM_UI_IsActive(void);

#ifdef __cplusplus
}
#endif

#endif /* __KK_PM_UI_H */
