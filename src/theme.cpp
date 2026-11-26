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
    /* selBg    */ 0x21CB,   /* #22395C 比 surface2 亮一档，焦点清楚 */
    /* osd      */ 0x08C5,   /* #0F1A2E OSD 底衬＝标题栏色 */
    /* osdTx    */ 0xEF7F,   /* #E8EEF9 */
    /* dark     */ true
};

/* 浅色配色：为 8px 字模做过对比度校准
 *   - muted 由 #6B7A93 加深到 #55627A（小字在白底上才不"发灰"）
 *   - header 不再纯白：白底页面里的顶/底栏会和卡片糊在一起，改成淡蓝灰
 *   - selBg 浅青 #D8EDF6：与 surface(#FFF) 明确区分，选中项一眼可见 */
const Theme themeLight = {
    /* bg       */ 0xF7BF,   /* #F2F5FA */
    /* surface  */ 0xFFFF,   /* #FFFFFF */
    /* surface2 */ 0xEF7E,   /* #E8EEF7 */
    /* header   */ 0xE75E,   /* #E4EBF6 */
    /* line     */ 0xCEDD,   /* #CFD9E8 */
    /* text     */ 0x1107,   /* #16203A */
    /* muted    */ 0x530F,   /* #55627A */
    /* accent   */ 0x0BB2,   /* #0E7490 */
    /* accentTx */ 0xFFFF,   /* #FFFFFF */
    /* ok       */ 0x0B4A,   /* #0B6B52 */
    /* warn     */ 0x9A40,   /* #9A4A05 */
    /* danger   */ 0xB123,   /* #B3261E */
    /* selBg    */ 0xDF7E,   /* #D8EDF6 */
    /* osd      */ 0xE75E,   /* #E4EBF6 OSD 底衬跟随主题（浅色=浅底深字） */
    /* osdTx    */ 0x1107,   /* #16203A */
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
