/*
 * theme.cpp  ——  主题状态与持久化
 * ---------------------------------------------------------------------
 * 用 NVS（Preferences）保存"深色/浅色"选择，掉电不丢，不依赖 SD 卡。
 * 默认深色：设备是相机，深底更耐看、夜间不刺眼，照片也更突出。
 * =================================================================== */
#include "theme.h"
#include <Preferences.h>

const Theme themeDark = {
    /* bg       */ 0x0884,   /* #0B1220 */
    /* surface  */ 0x10E6,   /* #131E36 */
    /* surface2 */ 0x1948,   /* #1B2946 */
    /* header   */ 0x08C5,   /* #0F1A2E */
    /* line     */ 0x2189,   /* #22304D */
    /* text     */ 0xEF7F,   /* #E8EEF9 */
    /* muted    */ 0x7C75,   /* #7C8CA8 */
    /* accent   */ 0x269D,   /* #22D3EE */
    /* accentTx */ 0x0146,   /* #062A33 */
    /* ok       */ 0x5E54,   /* #5DCAA5 */
    /* warn     */ 0xECE4,   /* #EF9F27 */
    /* danger   */ 0xF36D,   /* #F26D6D */
    /* dark     */ true
};

const Theme themeLight = {
    /* bg       */ 0xF7BF,   /* #F4F6FB */
    /* surface  */ 0xFFFF,   /* #FFFFFF */
    /* surface2 */ 0xEF9F,   /* #EEF2F9 */
    /* header   */ 0xFFFF,   /* #FFFFFF */
    /* line     */ 0xDEFD,   /* #D8DFEA */
    /* text     */ 0x1107,   /* #16203A */
    /* muted    */ 0x6BD2,   /* #6B7A93 */
    /* accent   */ 0x0BB2,   /* #0E7490 */
    /* accentTx */ 0xFFFF,   /* #FFFFFF */
    /* ok       */ 0x0B6A,   /* #0F6E56 */
    /* warn     */ 0xB281,   /* #B45309 */
    /* danger   */ 0xB8E3,   /* #B91C1C */
    /* dark     */ false
};

const Theme* g_th      = &themeDark;
bool         g_themeDark = true;

void theme_init(void)
{
    Preferences p;
    if (p.begin("ui", true)) {              /* 只读打开 */
        g_themeDark = p.getBool("dark", true);
        p.end();
    }
    g_th = g_themeDark ? &themeDark : &themeLight;
    Serial.printf("[UI] 主题：%s\n", g_themeDark ? "dark" : "light");
}

void theme_set_dark(bool dark)
{
    g_themeDark = dark;
    g_th = dark ? &themeDark : &themeLight;
    Preferences p;
    if (p.begin("ui", false)) {             /* 读写打开 */
        p.putBool("dark", dark);
        p.end();
    }
    Serial.printf("[UI] 主题切换 → %s\n", dark ? "dark" : "light");
}

void theme_toggle(void)
{
    theme_set_dark(!g_themeDark);
}
