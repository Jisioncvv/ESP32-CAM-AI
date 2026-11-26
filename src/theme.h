/*
 * =====================================================================
 * theme.h  ——  全局视觉规范（配色 + 通用控件 + 离屏画笔）
 * ---------------------------------------------------------------------
 * 设计目标：桌面 / 相机 / 相册 / 设置 / 游戏 / 备忘 用同一套颜色和
 *           同一套控件几何（标题栏 30px、底栏 20px、卡片圆角 10px），
 *           换 App 时视觉不跳变。
 *
 * 两套配色（运行时可切，存在 NVS，掉电保持）：
 *   Dark  —— 深蓝底 + 青色强调（默认）
 *   Light —— 浅灰底 + 深青强调
 *
 * 所有控件通过 Drawer 绘制：同一份代码既能画进 TFT_eSprite（整帧
 * 推屏防闪烁），也能退化直绘 tft。
 * =================================================================== */
#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>
#include "config.h"

/* --------------------- 配色表（RGB565） --------------------- */
struct Theme {
    uint16_t bg;        /* 页面底色 */
    uint16_t surface;   /* 卡片/列表行 */
    uint16_t surface2;  /* 次级块（棋盘、输入框） */
    uint16_t header;    /* 标题栏 / 底栏 */
    uint16_t line;      /* 分割线 / 边框 */
    uint16_t text;      /* 主文字 */
    uint16_t muted;     /* 次要文字 */
    uint16_t accent;    /* 强调色（选中、可点项） */
    uint16_t accentTx;  /* 强调色上的文字 */
    uint16_t ok;
    uint16_t warn;
    uint16_t danger;
    bool     dark;
};

extern const Theme  themeDark;
extern const Theme  themeLight;
extern const Theme* g_th;          /* 当前生效配色（所有 UI 只读它） */
extern bool         g_themeDark;

void theme_init(void);             /* 开机读 NVS，缺省深色 */
void theme_set_dark(bool dark);    /* 切换并落盘 */
void theme_toggle(void);

/* =====================================================================
 * Drawer：绘制目标包装（Sprite 优先，失败退化直绘）
 * =================================================================== */
struct Drawer {
    TFT_eSprite* s; TFT_eSPI* t;
    /* [根因修复] TFT_eSprite 没有重载 fillScreen()：继承版按 TFT_WIDTH/
     * TFT_HEIGHT(240x320) 填充，右侧 80 列会留黑。sprite 必须用 fillSprite */
    void fillScreen(uint16_t c)   { if (s) s->fillSprite(c);   else t->fillScreen(c); }
    void fillRect(int x,int y,int w,int h,uint16_t c) { if (s) s->fillRect(x,y,w,h,c); else t->fillRect(x,y,w,h,c); }
    void fillRoundRect(int x,int y,int w,int h,int r,uint16_t c) { if (s) s->fillRoundRect(x,y,w,h,r,c); else t->fillRoundRect(x,y,w,h,r,c); }
    void drawRoundRect(int x,int y,int w,int h,int r,uint16_t c) { if (s) s->drawRoundRect(x,y,w,h,r,c); else t->drawRoundRect(x,y,w,h,r,c); }
    void drawRect(int x,int y,int w,int h,uint16_t c) { if (s) s->drawRect(x,y,w,h,c); else t->drawRect(x,y,w,h,c); }
    void drawFastHLine(int x,int y,int w,uint16_t c) { if (s) s->drawFastHLine(x,y,w,c); else t->drawFastHLine(x,y,w,c); }
    void fillTriangle(int x0,int y0,int x1,int y1,int x2,int y2,uint16_t c) { if (s) s->fillTriangle(x0,y0,x1,y1,x2,y2,c); else t->fillTriangle(x0,y0,x1,y1,x2,y2,c); }
    void fillCircle(int x,int y,int r,uint16_t c) { if (s) s->fillCircle(x,y,r,c); else t->fillCircle(x,y,r,c); }

    void setTextColor(uint16_t fg, uint16_t bg) { if (s) s->setTextColor(fg,bg); else t->setTextColor(fg,bg); }
    void setTextDatum(uint8_t d)                { if (s) s->setTextDatum(d);    else t->setTextDatum(d); }
    void setTextFont(uint8_t f)                 { if (s) s->setTextFont(f);     else t->setTextFont(f); }
    void setTextSize(uint8_t sz)                { if (s) s->setTextSize(sz);    else t->setTextSize(sz); }
    void drawString(const String& str, int x, int y) {
        if (s) s->drawString(str, x, y); else t->drawString(str, x, y);
    }
    void drawCentreString(const String& str, int x, int y, uint8_t font) {
        if (s) s->drawCentreString(str, x, y, font); else t->drawCentreString(str, x, y, font);
    }

    /* 瓷片本体绘制（自动裁剪）：完整可见 → 圆角矩形；跨屏 → 只画屏内切片，
     * 实现自然滑出/滑入，杜绝 Sprite 越界写坏显存 */
    void tileBody(int x, int y, int w, int h, int r, uint16_t c) {
        int x0 = x < 0 ? 0 : x, x1 = (x + w > 320) ? 320 : x + w;
        int y0 = y < 0 ? 0 : y, y1 = (y + h > 240) ? 240 : y + h;
        if (x1 <= x0 || y1 <= y0) return;
        bool clipped = (x0 != x || x1 != x + w || y0 != y || y1 != y + h);
        if (!clipped && r > 0) fillRoundRect(x, y, w, h, r, c);
        else if (!clipped)     fillRect(x, y, w, h, c);
        else                   fillRect(x0, y0, x1 - x0, y1 - y0, c);
    }
};

/* --------------------- 通用控件（统一几何） --------------------- */
#define UI_HEADER_H  30
#define UI_FOOTER_H  20
#define UI_PAD       12

/* 标题栏：底色 header，左标题、右辅助信息，底部一条分割线 */
inline void ui_header(Drawer& g, const char* title, const char* right = nullptr)
{
    const Theme* th = g_th;
    g.fillRect(0, 0, 320, UI_HEADER_H, th->header);
    g.drawFastHLine(0, UI_HEADER_H - 1, 320, th->line);
    g.setTextDatum(TL_DATUM);
    g.setTextFont(1);
    g.setTextSize(1);
    g.setTextColor(th->text, th->header);
    g.drawString(title, UI_PAD, 11);
    if (right && right[0]) {
        g.setTextDatum(TR_DATUM);
        g.setTextColor(th->muted, th->header);
        g.drawString(right, 320 - UI_PAD, 11);
        g.setTextDatum(TL_DATUM);
    }
}

/* 底栏：按键提示（居中，弱化色） */
inline void ui_footer(Drawer& g, const char* hint)
{
    const Theme* th = g_th;
    int y = 240 - UI_FOOTER_H;
    g.fillRect(0, y, 320, UI_FOOTER_H, th->header);
    g.drawFastHLine(0, y, 320, th->line);
    g.setTextDatum(MC_DATUM);
    g.setTextFont(1);
    g.setTextSize(1);
    g.setTextColor(th->muted, th->header);
    g.drawString(hint, 160, y + 10);
    g.setTextDatum(TL_DATUM);
}

/* 列表行：选中 = 强调色 1px 描边 + 左侧 3px 竖条 */
inline void ui_row(Drawer& g, int x, int y, int w, int h, bool sel)
{
    const Theme* th = g_th;
    g.fillRoundRect(x, y, w, h, 10, sel ? th->surface2 : th->surface);
    if (sel) {
        g.drawRoundRect(x, y, w, h, 10, th->accent);
        g.fillRoundRect(x + 3, y + 8, 3, h - 16, 2, th->accent);
    }
}

/* 卡片底板 */
inline void ui_card(Drawer& g, int x, int y, int w, int h, uint16_t color)
{
    g.fillRoundRect(x, y, w, h, 10, color);
}

/* 进度条（内存占用等） */
inline void ui_bar(Drawer& g, int x, int y, int w, float ratio, uint16_t color)
{
    const Theme* th = g_th;
    if (ratio < 0) ratio = 0;
    if (ratio > 1) ratio = 1;
    g.fillRoundRect(x, y, w, 5, 2, th->surface2);
    int fw = (int)(w * ratio);
    if (fw >= 3) g.fillRoundRect(x, y, fw, 5, 2, color);
}
