/* kk_pm_ui.c —— 用 KK_UI 搭的功率计界面
 *
 * 页面结构（根页面是首页图标选择器）：
 *   HOME（4 个 32x32 图标）
 *     |- Monitor -> CUSTOM 页：当前 V / Vmax / I / Imax / P / PK
 *     |- Control -> MENU 页
 *     |    |- Peak clear -> ACTION，峰值清零（原按键短按）
 *     |    |- Zero cal   -> ACTION，重新做零点校准
 *     |    |- PWM: ON/OFF -> ACTION，PWM 输出启停（标签跟着实际状态变）
 *     |- About   -> INFO 页：设备与驱动状态
 *     |- PWM     -> CUSTOM 页：PWM 输出直通控制（大字通断 + 确定键切换）
 *
 * 本文件只描述界面与业务：不改 KK_UI 核心（只加了一个返回接口）、
 * 不额外分配帧缓冲、不使用动态内存；整屏刷新所有权完全交给 KK_UI。
 *
 * 按键归一化：KEY1(PA6) 短按 = 下一个、长按 = 确认；KEY2(PA7) 短按 = 返回。
 */
#include "kk_pm_ui.h"

#include "app.h"          /* 峰值 / 提示 / 清零峰值的读写口 */
#include "ina228.h"
#include "key.h"
#include "kk_oled.h"
#include "kk_pm_font.h"
#include "kk_pm_icons.h"
#include "kk_pm_splash.h"
#include "kk_ui.h"
#include "kk_ui_draw.h"

#include <string.h>

/* ======================= 版面常量 ======================= */

/* 屏幕 128x64，14 号字模，行距 16 像素。
   下面所有 x 坐标在绘制时都要再加上翻页偏移 x_offset */
#define PM_SCREEN_W     128         /* 屏宽(像素) */
#define PM_SCREEN_H     64          /* 屏高(像素) */
#define PM_LINE_H       16          /* 行距(像素) */
#define PM_TEXT_X       1           /* 正文左边界 */

/* 实时数据页四行的 y 坐标 */
#define PM_METER_V_Y    0
#define PM_METER_I_Y    PM_LINE_H
#define PM_METER_P_Y    (PM_LINE_H * 2)
#define PM_METER_PK_Y   (PM_LINE_H * 3)

/* 实时数据页: 功率那一行的反白圆角块, 以及块里文字的位置 */
#define PM_PWR_BOX_X    PM_TEXT_X
#define PM_PWR_BOX_Y    29
#define PM_PWR_BOX_W    126u
#define PM_PWR_BOX_H    19u
#define PM_PWR_BOX_R    4u
#define PM_PWR_TEXT_X   7

/* PWM 直通页的版面 */
#define PM_PWM_BOX_X    PM_TEXT_X
#define PM_PWM_BOX_Y    17
#define PM_PWM_BOX_W    126u
#define PM_PWM_BOX_H    22u
#define PM_PWM_BOX_R    5u
#define PM_PWM_ON_Y     20          /* 状态文字 */
#define PM_PWM_HINT_Y   44          /* 底部提示 */

/* 一次按键事件合成出来的“按住”时长。
   KK_UI 的按键消抖是 20ms, 只给一帧脉冲认不出来 */
#define PM_KEY_HOLD_MS  40u

/* ======================= 页面 ID 与事件 ID ======================= */

/* 两者都必须非零: KK_UI 用 routes[page - 1] 取页面描述, 0 留给 KK_UI_PAGE_NONE */
enum
{
  PAGE_HOME = 1,     /* 首页图标选择器(根页面) */
  PAGE_METER,        /* 自定义页: 实时 V / I / P / PK */
  PAGE_PWM,          /* 自定义页: PWM 输出直通控制 */
  PAGE_CONTROL,      /* 控制菜单 */
  PAGE_ABOUT         /* 关于页 */
};

enum
{
  EV_PEAK_CLEAR = 1, /* 峰值清零(原按键短按) */
  EV_ZERO_CAL,       /* 零点校准(原串口 C) */
  EV_PWM_TOGGLE      /* PWM 输出启停(原按键长按 / 串口 S、X) */
};

/* ======================= 内部状态 ======================= */

#define PM_PWM_LABEL_LEN   12u      /* "PWM: OFF" 加结束符 */
#define PM_INA_TEXT_LEN    8u       /* "FAIL" 加结束符 */
#define PM_STATE_UNKNOWN   0xFFu    /* 标签还没和实际状态同步过 */
#define PM_MENU_STATE_BYTES 2u      /* 菜单条目状态表: KK_UI 规定每项 2bit */

static uint8_t  ui_active;                  /* 1 = KK_UI 接管屏幕 */
static uint8_t  pending_keys;               /* 正在合成的 KK_UI 键值 */
static uint32_t pending_until;              /* 合成到什么时候为止 */
static uint8_t  back_request;               /* 有未完成的返回请求 */
static uint8_t  control_states[PM_MENU_STATE_BYTES];  /* 控制菜单条目状态表 */
static char     pwm_label[PM_PWM_LABEL_LEN] = "PWM: OFF"; /* PWM 条目的动态标签 */
static uint8_t  pwm_shown = PM_STATE_UNKNOWN; /* 标签当前显示的状态 */
static char     ina_text[PM_INA_TEXT_LEN] = "OK"; /* 关于页里的 INA228 状态 */

/* ======================= 页面描述 ======================= */

/* 根页面: 带大图标的首页选择器 */
static const KK_UI_HomeItem home_items[] = {
  {"Monitor", kk_pm_icons[0], PAGE_METER},
  {"Control", kk_pm_icons[1], PAGE_CONTROL},
  {"About",   kk_pm_icons[2], PAGE_ABOUT},
  {"PWM",     kk_pm_icons[3], PAGE_PWM}      /* PWM 输出直通入口 */
};

static const KK_UI_HomePage home_pages[] = {
  {home_items, (uint16_t)(sizeof(home_items) / sizeof(home_items[0]))}
};

/* 控制菜单: 本工程全部可操作功能都放在这里 */
static const KK_UI_MenuItem control_items[] = {
  {"Peak clear", KK_UI_MENU_ACTION, EV_PEAK_CLEAR},
  {"Zero cal",   KK_UI_MENU_ACTION, EV_ZERO_CAL},
  {pwm_label,    KK_UI_MENU_ACTION, EV_PWM_TOGGLE}   /* 标签动态显示 ON/OFF */
};

static const KK_UI_MenuPage menu_pages[] = {
  {"CONTROL", control_items, control_states,
   (uint16_t)(sizeof(control_items) / sizeof(control_items[0]))}
};

static const KK_UI_InfoRow about_rows[] = {
  {"Device",  "POWER METER"},
  {"Display", "KK_OLED 128x64"},
  {"Refresh", "Blocking"},
  {"INA228",  ina_text}
};

static const KK_UI_InfoPage info_pages[] = {
  {"About", about_rows, (uint16_t)(sizeof(about_rows) / sizeof(about_rows[0]))}
};

/* 页面路由表：PageId N 对应 routes[N-1] */
static const KK_UI_PageRoute routes[] = {
  {KK_UI_PAGE_HOME,   0u},   /* PAGE_HOME    */
  {KK_UI_PAGE_CUSTOM, 0u},   /* PAGE_METER   */
  {KK_UI_PAGE_CUSTOM, 1u},   /* PAGE_PWM     */
  {KK_UI_PAGE_MENU,   0u},   /* PAGE_CONTROL */
  {KK_UI_PAGE_INFO,   0u}    /* PAGE_ABOUT   */
};

static const KK_UI_App app = {
  PAGE_HOME,                 /* root_page */
  routes,                    /* routes */
  5u,                        /* route_count */
  home_pages, 1u,            /* 首页图标选择器 */
  menu_pages, 1u,            /* 菜单页 */
  info_pages, 1u,            /* 信息页 */
  2u,                        /* custom_page_count: 实时数据页 + PWM 页 */
  0, 0u,                     /* 不使用整数编辑 */
  0, 0u,                     /* 不使用开关编辑 */
  0, 0u,                     /* 不使用确认框 */
  {kk_font_pm, kk_font_pm, kk_font_pm},              /* 只有一个 14 号字模, 三种角色共用 */
  {"Back", "Cancel", "OK", "On", "Off", "Notice"}    /* 固定文案 */
};

/* ======================= 自定义页：实时数据 ======================= */

void KK_UI_CustomOnEnter(KK_UI_PageId page)
{
  (void)page;
}

void KK_UI_CustomOnLeave(KK_UI_PageId page)
{
  (void)page;
}

/* 自定义页的确定键：
     实时数据页(只读) -> 返回上级，也是没接 KEY2 时的退路
     PWM 页           -> 切换 PWM 输出 */
void KK_UI_CustomOnInput(KK_UI_PageId page, KK_UI_InputEvent event)
{
  if (event.action == KK_UI_INPUT_OK)
  {
    if (page == PAGE_PWM)
    {
      App_ToggleEsc();
    }
    else
    {
      (void)KK_UI_CustomRequestClose();
    }
  }
}

/* 实时数据每帧都要重画 */
bool KK_UI_CustomOnTick(KK_UI_PageId page, uint32_t now_ms)
{
  (void)page;
  (void)now_ms;
  return true;
}

/* 在屏宽内水平居中画一行文字(算上翻页偏移 x_offset) */
static void ui_centered(int16_t x_offset, int16_t y, const char *s)
{
  int16_t w = (int16_t)OLED_GetUTF8Width(s);
  int16_t x = (int16_t)((PM_SCREEN_W - w) / 2);

  (void)OLED_DrawUTF8((int16_t)(x + x_offset), y, s);
}

/* PWM 直通页: 大字显示通断状态, 确定键直接切换 */
static void ui_draw_pwm_page(int16_t x_offset)
{
  int16_t box_x = (int16_t)(x_offset + PM_PWM_BOX_X);
  uint8_t on    = App_GetEscOn();

  OLED_SetDrawMode(OLED_DRAW_SET);
  ui_centered(x_offset, 0, "PWM OUTPUT");

  /* 状态块: 输出中反白, 停止时只留边框 */
  if (on != 0u)
  {
    OLED_DrawRBox(box_x, PM_PWM_BOX_Y, PM_PWM_BOX_W, PM_PWM_BOX_H, PM_PWM_BOX_R);
    OLED_SetDrawMode(OLED_DRAW_CLEAR);   /* 文字从实心块里"挖"出来 */
  }
  else
  {
    OLED_DrawRFrame(box_x, PM_PWM_BOX_Y, PM_PWM_BOX_W, PM_PWM_BOX_H, PM_PWM_BOX_R);
  }
  ui_centered(x_offset, PM_PWM_ON_Y, (on != 0u) ? "PWM ON" : "PWM OFF");
  OLED_SetDrawMode(OLED_DRAW_SET);

  ui_centered(x_offset, PM_PWM_HINT_Y, "OK=Toggle");
}

/* 在正文左边界画一行文字 */
static void ui_line(int16_t x_offset, int16_t y, const char *s)
{
  (void)OLED_DrawUTF8((int16_t)(x_offset + PM_TEXT_X), y, s);
}

/* 拼出 "X=当前值 M=最大值" 这样一行: 当前值 3 位小数, 锁存最大值 2 位小数 */
static void ui_fmt_cur_max(char *dst, const char *label, float cur, float max)
{
  char sb[16];

  strcpy(dst, label);
  App_FmtFloat(sb, cur, 3);
  strcat(dst, sb);
  strcat(dst, " M=");
  App_FmtFloat(sb, max, 2);
  strcat(dst, sb);
}

void KK_UI_CustomOnDraw(KK_UI_PageId page, int16_t x_offset,
                        int16_t clip_x, uint16_t clip_width)
{
  char        sb[16], tmp[40];
  const char *msg;

  OLED_SetClipWindow(clip_x, 0, clip_width, PM_SCREEN_H);
  OLED_SetFont(kk_font_pm);
  OLED_SetFontPosition(OLED_FONT_POS_TOP);
  OLED_SetBackgroundMode(OLED_BG_TRANSPARENT);

  if (page == PAGE_PWM)
  {
    ui_draw_pwm_page(x_offset);
    return;
  }

  OLED_SetDrawMode(OLED_DRAW_SET);

  /* 第 1 行: 当前电压 + 锁存最大电压 */
  ui_fmt_cur_max(tmp, "V=", INA228_GetVoltage_V(), App_GetPeakVoltage_V());
  ui_line(x_offset, PM_METER_V_Y, tmp);

  /* 第 2 行: 当前电流(带符号) + 锁存最大电流 */
  ui_fmt_cur_max(tmp, "I=", INA228_GetCurrent_A(), App_GetPeakCurrent_A());
  ui_line(x_offset, PM_METER_I_Y, tmp);

  /* 第 3 行: 功率 —— 本机最核心的读数, 用反白圆角块突出 */
  strcpy(tmp, "P=");
  App_FmtFloat(sb, INA228_GetPower_W(), 3);
  strcat(tmp, sb);
  strcat(tmp, "W");
  OLED_DrawRBox((int16_t)(x_offset + PM_PWR_BOX_X),
                PM_PWR_BOX_Y, PM_PWR_BOX_W, PM_PWR_BOX_H, PM_PWR_BOX_R);
  OLED_SetDrawMode(OLED_DRAW_CLEAR);   /* 文字从实心块里"挖"出来 */
  (void)OLED_DrawUTF8((int16_t)(x_offset + PM_PWR_TEXT_X), PM_METER_P_Y, tmp);
  OLED_SetDrawMode(OLED_DRAW_SET);

  /* 第 4 行: 有临时提示先显示提示, 否则显示功率峰值 */
  msg = App_GetMessage();
  if (msg != 0)
  {
    ui_line(x_offset, PM_METER_PK_Y, msg);
  }
  else
  {
    strcpy(tmp, "PK=");
    App_FmtFloat(sb, App_GetPeakPower_W(), 3);
    strcat(tmp, sb);
    strcat(tmp, "W");
    ui_line(x_offset, PM_METER_PK_Y, tmp);
  }
}

/* ======================= 对外接口 ======================= */

uint8_t KK_PM_UI_IsActive(void)
{
  return ui_active;
}

void KK_PM_UI_ShowSplash(uint32_t hold_ms)
{
  OLED_SetDrawMode(OLED_DRAW_SET);
  OLED_SetBackgroundMode(OLED_BG_TRANSPARENT);
  OLED_ResetClipWindow();
  OLED_Clear();
  (void)OLED_DrawXBM(KK_PM_SPLASH_X, KK_PM_SPLASH_Y,
                     KK_PM_SPLASH_W, KK_PM_SPLASH_H, kk_pm_splash);
  (void)OLED_Update();
  HAL_Delay(hold_ms);
}

void KK_PM_UI_Init(uint32_t now_ms)
{
  pending_keys  = 0u;
  pending_until = now_ms;
  control_states[0] = 0u;
  control_states[1] = 0u;

  if (KK_UI_Init(&app) == KK_UI_OK)
  {
    ui_active = 1u;
  }
  else
  {
    /* 初始化失败就不接管屏幕；这时没有别的显示路径，屏幕停在开机图片，
       开机信息里会打印 KK_UI: INIT FAIL 便于定位 */
    ui_active = 0u;
  }
}

void KK_PM_UI_Update(uint32_t now_ms)
{
  KK_UI_Input input;
  KeyEvent_t  evt;

  input.keys          = 0u;
  input.encoder_delta = 0;

  /* 按键归一化：
       KEY1(PA6) 短按 -> DOWN，长按 -> OK
       KEY2(PA7) 短按 -> 返回（立即请求，不需要合成按住）
     一次只吃一个事件，其余留到下一轮，避免两个动作挤在一帧里 */
  evt = Key_GetEvent();
  if (evt == KEY2_SHORT)
  {
    back_request = 1u;
  }
  else if (evt == KEY1_SHORT)
  {
    pending_keys  = (uint8_t)KK_UI_KEY_DOWN;
    pending_until = now_ms + PM_KEY_HOLD_MS;
  }
  else if (evt == KEY1_LONG)
  {
    pending_keys  = (uint8_t)KK_UI_KEY_OK;
    pending_until = now_ms + PM_KEY_HOLD_MS;
  }

  if ((int32_t)(now_ms - pending_until) < 0)
  {
    input.keys = pending_keys;
  }

  /* 先把这一帧交给核心，再处理返回请求：
     这样 KK_UI 内部的 last_update 已经是最新的时间 */
  (void)KK_UI_Update(now_ms, input);

  /* 菜单里 PWM 那一条的标签跟着实际状态走, 状态变了才让核心重画 */
  {
    uint8_t on = App_GetEscOn();

    if (on != pwm_shown)
    {
      pwm_shown = on;
      strcpy(pwm_label, (on != 0u) ? "PWM: ON" : "PWM: OFF");
      KK_UI_Invalidate();
    }
  }

  if (back_request != 0u)
  {
    /* 正在翻页动画时核心返回 BUSY，留到下一轮再试 */
    if (KK_UI_NavigateBackRequest() != KK_UI_BUSY)
    {
      back_request = 0u;
    }
  }

  /* 关于页里的 INA228 状态跟着实际通信状态走, 变了才让核心重画 */
  {
    const char *st = (INA228_CommOk() != 0u) ? "OK" : "FAIL";

    if (strcmp(ina_text, st) != 0)
    {
      strcpy(ina_text, st);
      KK_UI_Invalidate();
    }
  }
}

void KK_PM_UI_ProcessEvents(void)
{
  KK_UI_EventId   ev;
  KK_UI_ErrorInfo err;

  while (KK_UI_PollEvent(&ev) != false)
  {
    switch (ev)
    {
      case EV_ZERO_CAL:
        if (INA228_CommOk() != 0u)
        {
          INA228_CalibrateZero();
        }
        break;

      case EV_PEAK_CLEAR:
        App_ClearPeaks();
        App_SetMessage("PEAK CLEARED");
        break;

      case EV_PWM_TOGGLE:
        App_ToggleEsc();
        break;

      default:
        break;
    }
  }

  /* 取走并丢弃 KK_UI 的错误信息，避免队列积压 */
  while (KK_UI_PollError(&err) != false)
  {
  }
}
