/*
 * =====================================================================
 * ui_app.cpp  ——  桌面 + App 实现（Phase 2）
 * ---------------------------------------------------------------------
 * displayTask = App 调度器：按 g_appMode 分派到各 App 主循环
 *   桌面       ui_desktop_loop      横向轮播（左右循环，中间放大立体）
 *   相机 App   lcd_app.h ui_camera_loop（lcd_app.cpp，预览渲染）
 *   相册 App   lcd_app.h ui_gallery_loop（lcd_app.cpp，列表/全屏）
 *   设置 App   ui_settings_loop     RAM/PSRAM/CPU/照片数/SD/WiFi 系统信息
 *   游戏 App   game_app_loop       选择页 + 俄罗斯方块 / 贪吃蛇 / 2048（game_app.cpp）
 *   备忘 App   ui_notes_loop        列表查看 + CAM 删除；Web 端 /notes 添加
 * 图标为程序化绘制（TFT_eSPI 基本图形拼装），无需额外图片素材
 * 桌面风格：白底 + iOS 彩色圆角瓷砖 + Font2 精致字体（LOAD_FONT2）
 * =====================================================================
 */
#include "config.h"
#include "ui_app.h"
#include "theme.h"
#include "lcd_app.h"
#include "camera_app.h"
#include "rtos_app.h"
#include "sd_app.h"
#include "web_app.h"
#include "game_app.h"
#include <SD.h>
#include <WiFi.h>
#include <time.h>

/* --------------------- 全局状态（ui_app.h extern） --------------------- */
volatile int  g_appMode    = MODE_DESKTOP;
volatile bool g_appExitReq = false;
volatile int  g_appSel     = 0;
volatile bool g_noteDeleteReq = false;   /* 备忘：删除当前选中请求 */
volatile bool g_wifiUiOpenReq = false;   /* 设置 App：打开 WiFi 管理界面请求 */

/* --------------------- App 名称与瓷砖强调色 ---------------- */
static const char* s_appNames[APP_COUNT] = {
    "Camera", "Album", "Settings", "Games", "Notes"
};
/* 深色底上够亮的强调色：青 / 薄荷 / 琥珀 / 洋红 / 天蓝 */
static const uint16_t s_appColors[APP_COUNT] = {
    0x269D, 0x5E54, 0xECE4, 0xF81F, 0x5C9F
};

void ui_app_init(void)
{
    Serial.println("[UI] 桌面 + App 框架就绪（主题化：深色/浅色可切）");
}


/* =====================================================================
 * 桌面图标绘制（程序化矢量图形，44x40 区域，左上角定位 x,y）
 *   fg = 强调色（每个 App 一个），bg = 瓷片底色（镂空用，深浅主题自动适配）
 * =================================================================== */
static void drawIconCamera(Drawer& g, int x, int y, uint16_t fg, uint16_t bg)
{
    g.fillRect(x + 8, y + 2, 12, 6, fg);            /* 顶部取景凸起 */
    g.fillRoundRect(x, y + 6, 44, 30, 4, fg);       /* 机身 */
    g.fillCircle(x + 22, y + 21, 10, bg);           /* 镜头外圈（镂空） */
    g.fillCircle(x + 22, y + 21, 6, fg);            /* 镜头内芯 */
}
static void drawIconAlbum(Drawer& g, int x, int y, uint16_t fg, uint16_t bg)
{
    g.fillRoundRect(x, y + 2, 44, 34, 3, fg);       /* 相框 */
    g.fillTriangle(x + 6, y + 30, x + 18, y + 12, x + 30, y + 30, bg);  /* 山 */
    g.fillTriangle(x + 16, y + 30, x + 30, y + 16, x + 40, y + 30, bg); /* 前山 */
    g.fillCircle(x + 34, y + 10, 4, bg);            /* 太阳（镂空） */
    g.fillCircle(x + 34, y + 10, 3, fg);
}
static void drawIconSetting(Drawer& g, int x, int y, uint16_t fg, uint16_t bg)
{
    for (int i = 0; i < 8; ++i) {                   /* 8 个齿 */
        float a = i * 0.7854f;                      /* 45° */
        int dx = (int)(cosf(a) * 14), dy = (int)(sinf(a) * 14);
        g.fillRect(x + 19 + dx - 3, y + 18 + dy - 3, 7, 7, fg);
    }
    g.fillCircle(x + 22, y + 20, 12, fg);           /* 齿轮盘 */
    g.fillCircle(x + 22, y + 20, 6, bg);            /* 中孔 */
}
static void drawIconGame(Drawer& g, int x, int y, uint16_t fg, uint16_t bg)
{
    g.fillRoundRect(x, y + 6, 44, 26, 8, fg);       /* 手柄机身 */
    g.fillRect(x + 8, y + 13, 10, 4, bg);           /* 十字键横 */
    g.fillRect(x + 11, y + 10, 4, 10, bg);          /* 十字键竖 */
    g.fillCircle(x + 31, y + 13, 3, bg);            /* A 钮 */
    g.fillCircle(x + 37, y + 19, 3, bg);            /* B 钮 */
}
static void drawIconNotes(Drawer& g, int x, int y, uint16_t fg, uint16_t bg)
{
    g.fillRect(x + 4, y + 2, 36, 36, fg);           /* 纸张 */
    for (int i = 0; i < 3; ++i)                     /* 三行字线 */
        g.fillRect(x + 9, y + 14 + i * 8, 26, 2, bg);
}

/* 图标分发表 */
typedef void (*IconDraw)(Drawer&, int, int, uint16_t, uint16_t);
static const IconDraw s_icons[APP_COUNT] = {
    drawIconCamera, drawIconAlbum, drawIconSetting, drawIconGame, drawIconNotes
};

/* =====================================================================
 * 桌面页绘制：横向轮播（carousel），支持滑动动画
 *   off = 轮播滚动位置（浮点，可为小数）；每个 App 瓷片按循环距离
 *   d（-2.5..2.5）连续映射：尺寸 92→44、间距 78px，两端贴边裁切
 *   顶栏纯色条（无文字），App 选中高光由"离中心最近"决定
 *   优先整帧绘制进相机 Sprite 再推屏（无闪烁）；Sprite 不可用则直绘
 * =================================================================== */
/* 桌面专用独立 Sprite：s_spr 被相机/相册路径复用后内部状态（宽高/视口）
 * 曾出现错乱（表现为右侧 1/3 黑屏、瓷片缺失/错位），桌面自建实例彻底隔离 */
static TFT_eSprite* s_deskSpr = nullptr;

/* =====================================================================
 * 桌面右上角状态栏：WiFi 信号强度（4 格）+ 当前时间（NTP 授时）
 *   未联网/未授时：信号 0 格灰、时间 "--:--"
 *   WiFi 连接后由 web_app_connect_sta() 内的 configTime 授时（北京时间）
 * =================================================================== */
static void draw_desktop_status(Drawer& g)
{
    const Theme* th = g_th;
    g.fillRect(196, 0, 124, 22, th->bg);        /* 状态区清底（重绘不残影） */

    /* ---- 时间 HH:MM ---- */
    struct tm tmv;
    char clockStr[8];
    if (getLocalTime(&tmv, 0))
        snprintf(clockStr, sizeof(clockStr), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    else
        strcpy(clockStr, "--:--");
    g.setTextDatum(TR_DATUM);
    g.setTextFont(1);
    g.setTextSize(1);
    g.setTextColor(th->text, th->bg);
    g.drawString(clockStr, 314, 8);
    g.setTextDatum(TL_DATUM);

    /* ---- WiFi 4 格信号柱（RSSI 分档） ---- */
    int level = 0;
    if (WiFi.status() == WL_CONNECTED) {
        long r = WiFi.RSSI();
        level = (r > -55) ? 4 : (r > -65) ? 3 : (r > -75) ? 2 : 1;
    }
    int bx = 248, base = 18;
    for (int i = 0; i < 4; ++i) {
        int h = 4 + i * 3;
        g.fillRect(bx + i * 5, base - h, 3, h, (i < level) ? th->ok : th->line);
    }
}

static void draw_desktop_frame(float off, int dbgStage = 0)
{
    const Theme* th = g_th;
    if (!s_deskSpr) {
        s_deskSpr = new TFT_eSprite(&tft);
        s_deskSpr->setColorDepth(16);
        if (!s_deskSpr->createSprite(320, 240)) {
            delete s_deskSpr;
            s_deskSpr = nullptr;
        }
    }
    TFT_eSprite* spr = s_deskSpr;
    Drawer g = { spr, &tft };          /* 桌面 Sprite 优先，失败退化直绘 */

    g.fillScreen(th->bg);
    if (dbgStage == 1) { if (spr) spr->pushSprite(0, 0); return; }   /* 调试：仅底色 */

    /* 左上角标识（弱化，替代原来的纯白空顶栏） */
    g.setTextDatum(TL_DATUM);
    g.setTextFont(1);
    g.setTextSize(1);
    g.setTextColor(th->muted, th->bg);
    g.drawString("ESP32-CAM", 12, 8);
    draw_desktop_status(g);                            /* 右上角：WiFi 信号 + 时间 */

    /* 瓷片布局（84/60/44 递进，间距 4px，静止时整体 6..314 对称居中）：
     * [TFT_eSprite 越界教训] Sprite 不裁剪越界绘图 → 必须靠 tileBody()
     * 逐帧裁剪。瓷片滑出屏幕边缘时只画屏内切片（自然滑出/滑入），
     * 滑到另一侧再自然进入 —— 循环连续，无瞬移、无越界 */
    int    sx[APP_COUNT], sy[APP_COUNT], ss[APP_COUNT];
    float  dd[APP_COUNT];
    int    order[APP_COUNT];
    for (int i = 0; i < APP_COUNT; ++i) {
        float d = fmodf(fmodf(i - off + 2.5f, 5.0f) + 5.0f, 5.0f) - 2.5f;
        float ad = fabsf(d);
        dd[i] = d;
        float ofs = (ad <= 1.0f) ? 76.0f * ad : 76.0f + 56.0f * (ad - 1.0f);
        int s = (ad <= 1.0f) ? (int)(84.0f - 24.0f * ad)
                             : (int)(60.0f - 16.0f * (ad - 1.0f));
        ss[i] = s;
        sx[i] = 160 + (int)(d < 0 ? -ofs : ofs) - s / 2;
        sy[i] = 116 - s / 2;
    }
    for (int a = 0; a < APP_COUNT; ++a) order[a] = a;   /* 选择排序：|d| 降序 */
    for (int a = 0; a < APP_COUNT; ++a)
        for (int b = a + 1; b < APP_COUNT; ++b)
            if (fabsf(dd[order[b]]) > fabsf(dd[order[a]])) {
                int t = order[a]; order[a] = order[b]; order[b] = t;
            }
    int center = 0;
    for (int i = 0; i < APP_COUNT; ++i)                 /* 离中心最近的做高光 */
        if (fabsf(dd[i]) < fabsf(dd[center])) center = i;

    for (int a = 0; a < APP_COUNT; ++a) {
        int i = order[a];
        int x = sx[i], y = sy[i], s = ss[i];
        bool full = (x >= 0 && x + s <= 320);           /* 完整可见才画图标/边框 */
        bool selp = (i == center);
        uint16_t tile = selp ? th->selBg : th->surface;
        g.tileBody(x, y, s, s, selp ? 12 : 10, tile);
        if (!th->dark && full)                          /* 浅色主题：瓷片需要描边 */
            g.drawRoundRect(x, y, s, s, selp ? 12 : 10, th->line);
        if (selp && full)
            g.drawRoundRect(x + 1, y + 1, s - 2, s - 2, 11, s_appColors[i]);
        if (full && dbgStage == 0)
            s_icons[i](g, x + (s - 44) / 2, y + (s - 40) / 2, s_appColors[i], tile);
    }

    if (dbgStage == 2) { if (spr) spr->pushSprite(0, 0); return; }   /* 调试：底色+瓷片 */

    /* 选中 App 名称 + 底部指示点 */
    g.setTextDatum(MC_DATUM);
    g.setTextFont(2);
    g.setTextSize(1);
    g.setTextColor(th->text, th->bg);
    g.drawString(s_appNames[center], 160, 176);

    for (int i = 0; i < APP_COUNT; ++i) {
        int px = 160 - (APP_COUNT - 1) * 9 + i * 18;
        if (i == center) g.fillRoundRect(px - 7, 214, 14, 4, 2, th->accent);
        else             g.fillCircle(px, 216, 2, th->line);
    }
    g.setTextDatum(TL_DATUM);

    if (spr) spr->pushSprite(0, 0);    /* 整帧一次推屏：无闪烁、动画连贯 */
}

/* =====================================================================
 * 桌面主循环：选择变化时播放滑动动画（7 帧整帧渲染，全部画进 Sprite
 * 含图标 → 无闪烁且内容完整）。TOP 单击=上一个（循环）、DOWN=下一个
 * =================================================================== */
void ui_desktop_loop(void)
{
    int lastSel = -1;
    const int STEPS = 7;                 /* 动画帧数 */
    uint32_t lastStatus = 0;
    while (true) {
        int sel = g_appSel;
        if (sel != lastSel) {
            /* 最短路径差值：-2..2（5 个 App 循环成圈） */
            int delta = ((sel - lastSel) % APP_COUNT + APP_COUNT) % APP_COUNT;
            if (lastSel >= 0 && delta > APP_COUNT / 2) delta -= APP_COUNT;
            if (lastSel < 0) {
                draw_desktop_frame((float)sel);          /* 首帧直接落位 */
            } else {
                for (int st = 1; st <= STEPS; ++st)      /* 平滑滑动 */
                    draw_desktop_frame((float)lastSel + delta * st / (float)STEPS);
            }
            lastSel = sel;
            lastStatus = millis();
        }
        if ((int)g_appMode != MODE_DESKTOP) return;   /* 已被 keyTask 切到某 App */
        /* 状态栏秒级局部刷新（时间走字 / 信号变化），不重画整页 */
        if (millis() - lastStatus >= 1000) {
            Drawer g = { nullptr, &tft };             /* 直绘到屏，不动瓷片 */
            draw_desktop_status(g);
            lastStatus = millis();
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* =====================================================================
 * WiFi 管理界面（设置 App 内子界面，CAM 单击进入）
 * ---------------------------------------------------------------------
 * 状态机：MENU 主菜单 → LIST 扫描选择 → KB 屏幕小键盘 → CONN 连接/结果
 * 按键：TOP=上/左移  DOWN=下/右移  CAM=确认  MID=返回上级
 *   （keyTask 在 MODE_SETTINGS 下：CAM→g_wifiUiOpenReq、MID→g_appExitReq、
 *    TOP/DOWN 单击→g_btTOPstate/g_btDownstate，本循环全部消费）
 * 连接成功后 SSID+密码 去重追加到 /wifi.txt（新条目排最前，wifista 优先试）
 * [输入法设计] 线性光标：全部按键串成一维序列，TOP=上一个 DOWN=下一个
 * （循环滚动），CAM=选中 —— 4 键即可完成全部导航与输入
 * [乱码教训] 每个界面绘制前必须显式复位 setTextSize/setTextFont，
 * 开机自检页遗留 setTextSize(2) 曾让设置页首次进入大字叠行像乱码
 * =================================================================== */
#define WIFISCAN_MAX   16
static String  s_wSsid[WIFISCAN_MAX];    /* 扫描结果：热点名 */
static int     s_wRssi[WIFISCAN_MAX];    /* 信号强度 */
static bool    s_wOpen[WIFISCAN_MAX];    /* 是否开放网络（免密码） */
static int     s_wCount = 0;
static int     s_wListTop = 0;           /* 列表滚动窗口顶 */
static String  s_wCurSsid;               /* 正在输入/连接的热点名 */
static String  s_wPass;                  /* 小键盘输入中的密码 */
static int     s_kbCur    = 3;           /* 小键盘当前光标（线性序号） */
static int     s_kbPage   = 0;           /* 字符页：0=数字(默认) 1=abc 2=ABC 3=SYM */

/* /wifi.txt 去重保存：新条目插最前（wifista 轮流连接优先试最新的） */
static void wifi_save_credential(const String& ssid, const String& pass)
{
    String old[8]; int n = 0;
    File f = SD.open("/wifi.txt", FILE_READ);
    if (f) {
        while (f.available() && n < 8) {
            String l = f.readStringUntil('\n'); l.trim();
            if (l.length()) old[n++] = l;
        }
        f.close();
    }
    String newline = ssid + " " + pass;
    String out = newline + "\n";
    for (int i = 0; i < n; ++i) {
        int sp = old[i].indexOf(' ');
        if (sp > 0 && old[i].substring(0, sp) == ssid) continue;   /* 同名旧条目丢弃 */
        if (old[i] == newline) continue;
        out += old[i] + "\n";
    }
    SD.remove("/wifi.txt");
    File w = SD.open("/wifi.txt", FILE_WRITE);
    if (w) { w.print(out); w.close(); }
    Serial.printf("[WIFI] 已记忆热点: %s\n", ssid.c_str());
}

/* ---- 小键盘：线性序号 0..44 → 屏幕位置 ----
 *   0..2   功能行 y44..68：DEL / CLR / 页面循环（3 键 x 106px）
 *   3..42  字符 4 行 x 10 列（键 32x24）
 *   43/44  底部 Cancel / Connect（150x28）
 * [用户反馈] 密码多为纯数字：默认页=数字，abc 键循环 数字→字母→符号；
 * TOP/DOWN 长按自动连发（accelerated），解决逐键移动太慢的问题 */
static void kb_key_rect(int k, int& x, int& y, int& w, int& h)
{
    if (k < 3)      { x = k * 106;     y = 44;  w = 106; h = 24; }
    else if (k < 43){ int i = k - 3;   x = (i % 10) * 32;
                      y = 72 + (i / 10) * 26; w = 32; h = 24; }
    else            { x = (k == 43) ? 0 : 170; y = 182; w = 150; h = 28; }
}

static String kb_key_label(int k)
{
    if (k < 3) {
        if (k == 0) return "DEL";
        if (k == 1) return "CLR";
        static const char* nextPage[4] = { "abc", "ABC", "SYM", "123" };
        return nextPage[s_kbPage];      /* 标签=按下后将切换到的页 */
    }
    if (k >= 43) return (k == 43) ? "Cancel" : "Connect";
    int i = k - 3, row = i / 10, col = i % 10;
    /* 四页字符表：数字（默认，密码多为纯数字）/ 小写 / 大写 / 符号 */
    static const char* const pg[4][4] = {
        { "1234567890", "-_.@#!*+=&", "()[]{}:;,~", "\"'<>?/\\|`^" },
        { "abcdefghij", "klmnopqrst", "uvwxyz0123", "456789_-.@" },
        { "ABCDEFGHIJ", "KLMNOPQRST", "UVWXYZ0123", "456789_-.@" },
        { "!@#$%&*()-", "+=[]{};:'\"", "<>?/\\|`^,", "0123456789" }
    };
    return String(pg[s_kbPage][row][col]);
}

/* =====================================================================
 * WiFi 子界面绘制（全部走主题 token + Sprite 合成）
 * ---------------------------------------------------------------------
 * [历史 bug] 主题迁移时把这里改成了 "fg=muted / bg=text" 的写法：
 *   浅色主题下 text 是深藏青 → 文字后面出现一块深色补丁，
 *   再配上 muted 灰字直接看不清；小键盘选中键更离谱，fg 和 bg 都取了
 *   text（同色），按下的键整个"消失"。
 * 现在统一为：文字 = text/muted，底色 = bg/selBg/surface，选中 = accent。
 * 另外改为画进复用 Sprite 再整帧推屏，和设置/游戏/备忘同一条路径。
 * =================================================================== */
static inline Drawer ui_spr(void)
{
    return Drawer{ lcd_app_get_spr(), &tft };
}
static inline void ui_push(Drawer& g)
{
    if (g.s) g.s->pushSprite(0, 0);
}

/* 画单个按键：选中=强调色实心，Connect=成功色，其余=卡片白底 + 描边 */
static void kb_draw_key(Drawer& g, int k, bool sel)
{
    int x, y, w, h; kb_key_rect(k, x, y, w, h);
    const Theme* th = g_th;
    uint16_t bg, fg;
    if (sel)          { bg = th->accent;   fg = th->accentTx; }
    else if (k == 44) { bg = th->ok;       fg = th->accentTx; }
    else if (k == 43) { bg = th->surface2; fg = th->muted; }
    else              { bg = th->surface;  fg = th->text; }

    g.fillRect(x, y, w, h, th->bg);          /* 先清底：圆角处不留旧色 */
    g.fillRoundRect(x + 1, y + 1, w - 2, h - 2, 5, bg);
    if (k != 44) g.drawRoundRect(x + 1, y + 1, w - 2, h - 2, 5, th->line);
    /* [字位偏移根因] 旧代码用 drawCentreString(label, x+w/2, y+h/2, 2)：
     * TFT_eSPI 的这个函数内部强制 TC_DATUM（y 是"文字顶边"而不是中心），
     * 传 y+h/2 等于把 16px 字模的顶部摆到按键垂直中心 → 整排文字下移
     * 8px，压到按键下边框上，看起来就是"字和键对不上"。
     * 改用 MC_DATUM + drawString 才是真正的水平/垂直居中；字模也必须
     * 显式指定 font2，否则会继承上一个界面留下的字号。 */
    g.setTextFont(2);
    g.setTextSize(1);
    g.setTextColor(fg, bg);
    g.setTextDatum(MC_DATUM);
    g.drawString(kb_key_label(k), x + w / 2, y + h / 2);
    g.setTextDatum(TL_DATUM);
}

/* 密码行原位刷新（卡片底 + 主文字，光标用强调色竖条） */
static void kb_draw_pass(Drawer& g)
{
    const Theme* th = g_th;
    g.fillRect(0, 24, 320, 21, th->surface);
    g.drawFastHLine(0, 45, 320, th->line);
    g.setTextFont(2);
    g.setTextSize(1);
    g.setTextColor(th->text, th->surface);
    g.setCursor(6, 28);
    String d = s_wPass;
    if (d.length() > 25) d = d.substring(d.length() - 25);   /* 长密码显示尾部 */
    g.print(d);
    int cx = 6 + g.textWidth(d);
    if (cx > 310) cx = 310;
    g.fillRect(cx + 1, 28, 2, 14, th->accent);              /* 光标 */
}

/* 小键盘整屏：标题栏 + 密码行 + 45 键 + 底栏提示 */
static void kb_draw_screen(void)
{
    Drawer g = ui_spr();
    const Theme* th = g_th;
    g.fillScreen(th->bg);
    g.fillRect(0, 0, 320, 24, th->header);
    g.drawFastHLine(0, 23, 320, th->line);
    g.setTextSize(1);
    g.setTextDatum(TL_DATUM);
    g.setTextFont(2);
    g.setTextColor(th->text, th->header);
    g.setCursor(10, 5);
    String t = s_wCurSsid;
    if (t.length() > 24) t = t.substring(0, 24);
    g.print("Password: " + t);

    kb_draw_pass(g);
    for (int k = 0; k < 45; ++k) kb_draw_key(g, k, k == s_kbCur);

    g.fillRect(0, 216, 320, 24, th->header);
    g.drawFastHLine(0, 216, 320, th->line);
    g.setTextFont(1);
    g.setTextColor(th->muted, th->header);
    g.setTextDatum(MC_DATUM);
    g.drawString("TOP/DN move   CAM select   MID cancel", 160, 228);
    g.setTextDatum(TL_DATUM);
    ui_push(g);
}

/* ---- 扫描 / 连接 / 结果页 ---- */
static void wifi_page_header(Drawer& g, const char* title, uint16_t bgc, uint16_t fgc)
{
    const Theme* th = g_th;
    g.fillRect(0, 0, 320, 24, bgc);
    g.drawFastHLine(0, 23, 320, th->line);
    g.setTextFont(2);
    g.setTextSize(1);
    g.setTextDatum(TL_DATUM);
    g.setTextColor(fgc, bgc);
    g.setCursor(10, 5);
    g.print(title);
}

static void wifi_page_footer(Drawer& g, const char* hint)
{
    const Theme* th = g_th;
    g.fillRect(0, 216, 320, 24, th->header);
    g.drawFastHLine(0, 216, 320, th->line);
    g.setTextFont(1);
    g.setTextColor(th->muted, th->header);
    g.setTextDatum(MC_DATUM);
    g.drawString(hint, 160, 228);
    g.setTextDatum(TL_DATUM);
}

static void wifi_scan_wait_draw(void)
{
    Drawer g = ui_spr();
    const Theme* th = g_th;
    g.fillScreen(th->bg);
    wifi_page_header(g, "WiFi Setup", th->header, th->text);
    g.setTextFont(2);
    g.setTextDatum(MC_DATUM);
    g.setTextColor(th->text, th->bg);
    g.drawString("Scanning WiFi...", 160, 100);
    g.setTextFont(1);
    g.setTextColor(th->muted, th->bg);
    g.drawString("2.4GHz, about 3 seconds", 160, 128);
    g.setTextDatum(TL_DATUM);
    ui_push(g);
}

static void wifi_conn_draw(const String& ssid)
{
    Drawer g = ui_spr();
    const Theme* th = g_th;
    g.fillScreen(th->bg);
    wifi_page_header(g, "Connecting...", th->header, th->text);
    g.setTextFont(2);
    g.setTextDatum(MC_DATUM);
    g.setTextColor(th->text, th->bg);
    String t = ssid;
    if (t.length() > 20) t = t.substring(0, 20);
    g.drawString(t, 160, 104);
    g.setTextFont(1);
    g.setTextColor(th->muted, th->bg);
    g.drawString("please wait (up to 15s)", 160, 130);
    g.setTextDatum(TL_DATUM);
    ui_push(g);
}

static void wifi_result_draw(bool ok, const String& info)
{
    Drawer g = ui_spr();
    const Theme* th = g_th;
    g.fillScreen(th->bg);
    wifi_page_header(g, ok ? "Connected OK" : "Connect FAILED",
                     ok ? th->ok : th->danger, th->accentTx);
    g.setTextDatum(MC_DATUM);
    if (ok) {
        g.setTextFont(2);
        g.setTextColor(th->text, th->bg);
        g.drawString("IP: " + info, 160, 100);
        g.setTextFont(1);
        g.setTextColor(th->muted, th->bg);
        g.drawString("Browser (same WiFi) -> this IP", 160, 132);
    } else {
        g.setTextFont(2);
        g.setTextColor(th->danger, th->bg);
        g.drawString(info, 160, 108);
    }
    g.setTextDatum(TL_DATUM);
    wifi_page_footer(g, "CAM/MID = back");
    ui_push(g);
}

/* ---- WiFi 主菜单 ---- */
static void wifi_menu_draw(int sel)
{
    Drawer g = ui_spr();
    const Theme* th = g_th;
    g.fillScreen(th->bg);
    wifi_page_header(g, "WiFi Setup", th->header, th->text);

    bool apOn = (WiFi.getMode() & WIFI_MODE_AP);
    const char* items[3] = { "Scan & Connect", "Connect Saved",
                             apOn ? "Hotspot Mode : ON" : "Hotspot Mode : OFF" };
    for (int i = 0; i < 3; ++i) {
        int y = 40 + i * 40;
        uint16_t rowBg = (i == sel) ? th->selBg : th->surface;
        g.fillRoundRect(10, y, 300, 32, 8, rowBg);
        if (i == sel) {
            g.drawRoundRect(10, y, 300, 32, 8, th->accent);
            g.fillRoundRect(14, y + 10, 3, 12, 2, th->accent);
        } else {
            g.drawRoundRect(10, y, 300, 32, 8, th->line);
        }
        g.setTextFont(2);
        g.setTextSize(1);
        g.setTextColor(i == sel ? th->text : th->muted, rowBg);
        g.setCursor(28, y + 8);
        g.print(items[i]);
        if (i == 2)                                   /* 状态点：开=绿 关=灰 */
            g.fillCircle(296, y + 16, 4, apOn ? th->ok : th->line);
    }

    g.setTextFont(1);
    g.setTextColor(th->muted, th->bg);
    g.setCursor(14, 168);
    g.print("WiFi: " + web_app_wifi_status_text());

    wifi_page_footer(g, "TOP/DN move   CAM select   MID back");
    ui_push(g);
}

/* ---- 扫描结果列表 ---- */
static void wifi_list_draw(int sel)
{
    Drawer g = ui_spr();
    const Theme* th = g_th;
    g.fillScreen(th->bg);
    wifi_page_header(g, ("WiFi List  " + String(s_wCount) + " found").c_str(),
                     th->header, th->text);

    if (s_wCount == 0) {
        g.setTextFont(2);
        g.setTextDatum(MC_DATUM);
        g.setTextColor(th->muted, th->bg);
        g.drawString("No networks", 160, 108);
        g.setTextDatum(TL_DATUM);
        wifi_page_footer(g, "MID = back");
        ui_push(g);
        return;
    }
    if (sel < s_wListTop) s_wListTop = sel;
    if (sel > s_wListTop + 7) s_wListTop = sel - 7;
    if (s_wListTop < 0) s_wListTop = 0;
    for (int i = 0; i < 8 && s_wListTop + i < s_wCount; ++i) {
        int idx = s_wListTop + i;
        int y = 28 + i * 23;
        bool s = (idx == sel);
        uint16_t rowBg = s ? th->selBg : th->bg;
        if (s) {
            g.fillRoundRect(6, y, 308, 21, 6, rowBg);
            g.drawRoundRect(6, y, 308, 21, 6, th->accent);
        }
        String nm = s_wSsid[idx];
        if (nm.length() > 20) nm = nm.substring(0, 20);
        g.setTextFont(1);
        g.setTextSize(1);
        g.setTextColor(th->text, rowBg);
        g.setCursor(14, y + 7);
        g.print(nm.length() ? nm : String("(hidden)"));
        g.setTextColor(s ? th->accent : th->muted, rowBg);
        g.setCursor(226, y + 7);
        g.print(s_wOpen[idx] ? "OPEN" : "PWD");
        g.setCursor(266, y + 7);
        g.print(String(s_wRssi[idx]));
    }

    wifi_page_footer(g, "TOP/DN move   CAM connect   MID back");
    ui_push(g);
}

/* [长按加速] TOP/DOWN 原始电平轮询：首次按下立即移动 1 步；
 * 按住超 500ms 后每 80ms 连发一步。绕过 keyTask 双击判定延迟，
 * 单击也即刻响应（keyTask 的单击标志在本界面直接丢弃） */
static bool nav_step(uint8_t pin, uint32_t& holdT0, uint32_t& lastStep)
{
    bool pressed = (digitalRead(pin) == LOW);   /* 按键低有效 */
    uint32_t now = millis();
    if (!pressed) { holdT0 = 0; return false; }
    if (holdT0 == 0) { holdT0 = now; lastStep = now; return true; }
    if (now - holdT0 > 500 && now - lastStep >= 80) { lastStep = now; return true; }
    return false;
}

/* WiFi 管理子界面主循环（MID=返回上级；菜单态 MID=回设置信息页） */
static void ui_wifi_loop(void)
{
    const int W_MENU = 0, W_LIST = 1, W_KB = 2, W_CONN = 3;
    int state = W_MENU;
    int menuSel = 0, listSel = 0;
    uint32_t tHold = 0, tLast = 0, dHold = 0, dLast = 0;

    wifi_menu_draw(menuSel);
    while (true) {
        /* ---- MID（keyTask 置 g_appExitReq）= 返回上级 ---- */
        if (g_appExitReq) {
            g_appExitReq = false;
            if (state == W_MENU) return;                 /* 回设置信息页 */
            if (state == W_KB)   { state = W_LIST; wifi_list_draw(listSel); }
            else                 { state = W_MENU; wifi_menu_draw(menuSel); }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        /* ---- CAM（g_wifiUiOpenReq）= 确认 ---- */
        if (g_wifiUiOpenReq) {
            g_wifiUiOpenReq = false;
            if (state == W_MENU) {
                if (menuSel == 0) {                      /* 扫描周围热点 */
                    wifi_scan_wait_draw();
                    WiFi.mode(WIFI_STA);
                    int n = WiFi.scanNetworks();
                    s_wCount = 0;
                    for (int i = 0; i < n && s_wCount < WIFISCAN_MAX; ++i) {
                        s_wSsid[s_wCount] = WiFi.SSID(i);
                        s_wRssi[s_wCount] = WiFi.RSSI(i);
                        s_wOpen[s_wCount] =
                            (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
                        ++s_wCount;
                    }
                    WiFi.scanDelete();
                    listSel = 0; s_wListTop = 0;
                    state = W_LIST;
                    wifi_list_draw(listSel);
                } else if (menuSel == 1) {               /* 重连已存热点 */
                    wifi_conn_draw("saved list");
                    bool ok = false;
                    File f = SD.open("/wifi.txt", FILE_READ);
                    if (f) {
                        while (f.available() && !ok) {
                            String l = f.readStringUntil('\n'); l.trim();
                            int sp = l.indexOf(' ');
                            if (sp <= 0) continue;
                            ok = web_app_connect_ssid(l.substring(0, sp),
                                                      l.substring(sp + 1), 12000);
                        }
                        f.close();
                    }
                    wifi_result_draw(ok, ok ? WiFi.localIP().toString()
                                            : "no saved network works");
                    state = W_CONN;
                } else {                                 /* 热点模式开关 */
                    web_app_toggle_wifi();
                    wifi_menu_draw(menuSel);
                }
            } else if (state == W_LIST) {
                if (s_wCount == 0) { continue; }
                s_wCurSsid = s_wSsid[listSel];
                if (s_wOpen[listSel]) {                  /* 开放网络：免密码直连 */
                    wifi_conn_draw(s_wCurSsid);
                    bool ok = web_app_connect_ssid(s_wCurSsid, "", 12000);
                    if (ok) wifi_save_credential(s_wCurSsid, "");
                    wifi_result_draw(ok, ok ? WiFi.localIP().toString()
                                            : "connect failed");
                    state = W_CONN;
                } else {                                 /* 加密网络：进小键盘 */
                    s_wPass = "";
                    s_kbCur = 3;
                    s_kbPage = 0;                        /* 默认数字页 */
                    state = W_KB;
                    kb_draw_screen();
                }
            } else if (state == W_KB) {                  /* 小键盘：选中键生效 */
                if (s_kbCur == 0) {                      /* DEL 退格 */
                    if (s_wPass.length()) s_wPass.remove(s_wPass.length() - 1);
                    Drawer kg = ui_spr(); kb_draw_pass(kg); ui_push(kg);
                } else if (s_kbCur == 1) {               /* CLR 清空 */
                    s_wPass = "";
                    Drawer kg = ui_spr(); kb_draw_pass(kg); ui_push(kg);
                } else if (s_kbCur == 2) {               /* 页面循环：数字→abc→ABC→SYM */
                    s_kbPage = (s_kbPage + 1) % 4;
                    kb_draw_screen();
                } else if (s_kbCur < 43) {               /* 字符键（密码上限 32） */
                    if (s_wPass.length() < 32) {
                        s_wPass += kb_key_label(s_kbCur);
                        Drawer kg = ui_spr(); kb_draw_pass(kg); ui_push(kg);
                    }
                } else if (s_kbCur == 43) {              /* Cancel 回列表 */
                    state = W_LIST;
                    wifi_list_draw(listSel);
                } else {                                 /* Connect 开始连接 */
                    wifi_conn_draw(s_wCurSsid);
                    bool ok = web_app_connect_ssid(s_wCurSsid, s_wPass, 15000);
                    if (ok) wifi_save_credential(s_wCurSsid, s_wPass);
                    wifi_result_draw(ok, ok ? WiFi.localIP().toString()
                                            : "check password / signal");
                    state = W_CONN;
                }
            } else {                                     /* W_CONN 结果页 → 回菜单 */
                wifi_menu_draw(menuSel);
                state = W_MENU;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        /* ---- TOP / DOWN = 光标移动（原始电平轮询 + 长按连发加速） ---- */
        g_btTOPstate  = 0;               /* keyTask 单击标志弃用，防双重移动 */
        g_btDownstate = 0;
        bool up   = nav_step(PIN_KEY_TOP,  tHold, tLast);
        bool down = nav_step(PIN_KEY_DOWN, dHold, dLast);
        if (up || down) {
            int dir = down ? 1 : -1;
            if (state == W_MENU) {
                menuSel = (menuSel + dir + 3) % 3;
                wifi_menu_draw(menuSel);
            } else if (state == W_LIST && s_wCount) {
                listSel = (listSel + dir + s_wCount) % s_wCount;
                wifi_list_draw(listSel);
            } else if (state == W_KB) {
                int old = s_kbCur;
                s_kbCur = (s_kbCur + dir + 45) % 45;
                Drawer kg = ui_spr();
                kb_draw_key(kg, old, false);
                kb_draw_key(kg, s_kbCur, true);
                ui_push(kg);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* =====================================================================
 * 设置 App：系统信息 + 主题切换
 * ---------------------------------------------------------------------
 * 版式：内存卡片（RAM/PSRAM 进度条）→ 系统信息卡片 → WiFi 行 → 两个操作
 * 按键：TOP/DOWN 在两个操作间切换焦点，CAM 执行（WiFi 设置 / 换主题）
 * 每秒整帧重画进 Sprite 再推屏：数值刷新不闪、也不会留残影
 * =================================================================== */
static int s_setSel = 0;                 /* 0=WiFi setup  1=Theme */

static void settings_draw(Drawer& g)
{
    const Theme* th = g_th;
    g.fillScreen(th->bg);
    ui_header(g, "Settings", "v3.0");

    /* ---- 内存卡片 ---- */
    uint32_t hT = ESP.getHeapSize(),  hF = ESP.getFreeHeap();
    uint32_t pT = ESP.getPsramSize(), pF = ESP.getFreePsram();
    ui_card(g, 10, 36, 300, 68, th->surface);
    g.setTextFont(1);
    g.setTextSize(1);
    g.setTextDatum(TL_DATUM);
    g.setTextColor(th->muted, th->surface);
    g.drawString("RAM", 22, 44);
    g.setTextDatum(TR_DATUM);
    g.setTextColor(th->text, th->surface);
    g.drawString(String((hT - hF) / 1024) + " / " + String(hT / 1024) + " KB", 298, 44);
    ui_bar(g, 22, 60, 276, hT ? (float)(hT - hF) / (float)hT : 0, th->accent);

    g.setTextDatum(TL_DATUM);
    g.setTextColor(th->muted, th->surface);
    g.drawString("PSRAM", 22, 76);
    g.setTextDatum(TR_DATUM);
    g.setTextColor(th->text, th->surface);
    g.drawString(String((pT - pF) / 1024) + " / " + String(pT / 1024) + " KB", 298, 76);
    ui_bar(g, 22, 92, 276, pT ? (float)(pT - pF) / (float)pT : 0, th->ok);

    /* ---- 系统信息卡片 ---- */
    ui_card(g, 10, 108, 300, 64, th->surface);
    auto row = [&](int i, const char* label, const String& value, uint16_t vc) {
        int y = 116 + i * 14;
        g.setTextDatum(TL_DATUM);
        g.setTextColor(th->muted, th->surface);
        g.drawString(label, 22, y);
        g.setTextDatum(TR_DATUM);
        g.setTextColor(vc, th->surface);
        g.drawString(value, 298, y);
    };
    uint32_t up = millis() / 1000;
    char upt[20];
    snprintf(upt, sizeof(upt), "%lu:%02lu:%02lu",
             (unsigned long)(up / 3600), (unsigned long)((up / 60) % 60),
             (unsigned long)(up % 60));
    row(0, "CPU",      String(ESP.getCpuFreqMHz()) + " MHz", th->text);
    row(1, "Uptime",   String(upt), th->text);

    static int photos = -1;
    static uint32_t lastScan = 0;
    if (photos < 0 || millis() - lastScan > 5000) {
        photos = 0;
        File root = SD.open("/");
        if (root) {
            for (File f = root.openNextFile(); f; f = root.openNextFile()) {
                if (!f.isDirectory() && String(f.name()).endsWith(".jpg")) photos++;
                f.close();
            }
            root.close();
        }
        lastScan = millis();
    }
    row(2, "Photos", String(photos), th->text);
    row(3, "SD card", sd_app_is_ready() ? "ready" : "missing",
        sd_app_is_ready() ? th->ok : th->danger);

    /* ---- WiFi 行 ---- */
    g.setTextDatum(TL_DATUM);
    g.setTextColor(th->muted, th->bg);
    g.drawString("WiFi", 22, 180);
    g.setTextDatum(TR_DATUM);
    String ws = web_app_wifi_status_text();
    g.setTextColor(ws.indexOf("OFF") >= 0 ? th->muted : th->warn, th->bg);
    g.drawString(ws, 298, 180);

    /* ---- 两个操作按钮 ---- */
    g.setTextDatum(MC_DATUM);
    g.setTextFont(1);
    g.setTextSize(1);
    const char* act[2] = { "WiFi setup", g_themeDark ? "Theme: dark" : "Theme: light" };
    for (int i = 0; i < 2; ++i) {
        int x = i ? 162 : 10;
        bool on = (s_setSel == i);
        g.fillRoundRect(x, 198, 148, 22, 8, on ? th->selBg : th->surface);
        g.drawRoundRect(x, 198, 148, 22, 8, on ? th->accent : th->line);
        g.setTextColor(on ? th->accent : th->muted, on ? th->selBg : th->surface);
        g.drawString(act[i], x + 74, 209);
    }
    g.setTextDatum(TL_DATUM);
}

static void ui_settings_loop(void)
{
    Drawer g = { lcd_app_get_spr(), &tft };
    auto redraw = [&]() {
        settings_draw(g);
        if (g.s) g.s->pushSprite(0, 0);
    };
    s_setSel = 0;
    redraw();
    uint32_t lastUpd = millis();
    while (!g_appExitReq) {
        if (g_wifiUiOpenReq) {                     /* CAM 单击：执行当前焦点项 */
            g_wifiUiOpenReq = false;
            if (s_setSel == 0) {
                ui_wifi_loop();
                g_btTOPstate = g_btDownstate = 0;  /* 清子界面残留按键标志 */
            } else {
                theme_toggle();                    /* 深/浅色切换，NVS 掉电保持 */
            }
            redraw();
            lastUpd = millis();
            continue;
        }
        if (g_btTOPstate || g_btDownstate) {       /* TOP/DOWN 换焦点 */
            g_btTOPstate = g_btDownstate = 0;
            s_setSel ^= 1;
            redraw();
            continue;
        }
        uint32_t now = millis();
        if (now - lastUpd >= 1000) {               /* 1 秒整帧刷新 */
            redraw();
            lastUpd = now;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    g_appExitReq = false;
    g_btTOPstate = g_btDownstate = 0;
}

/* =====================================================================
 * 备忘 App：/notes.txt 每行一条（Web 端 /notes 页面添加）
 *   TOP/DOWN 移动选中；CAM 删除当前选中；MID 退出
 * 版式：卡片式列表，选中行强调色描边 + 左侧竖条（与游戏菜单同款）
 * =================================================================== */
static void ui_notes_loop(void)
{
    static const int MAX_NOTES = 50;
    static const int VISIBLE   = 7;
    String lines[50];
    int n = 0, sel = 0, top = 0;

    auto reload = [&]() {
        n = 0;
        if (!SD.exists("/notes.txt")) {           /* 首次使用：建空文件，避免 vfs 报错 */
            File nf = SD.open("/notes.txt", FILE_WRITE);
            if (nf) nf.close();
        }
        File f = SD.open("/notes.txt", FILE_READ);
        if (f) {
            while (f.available() && n < MAX_NOTES) {
                String l = f.readStringUntil('\n');
                l.trim();
                if (l.length()) lines[n++] = l;
            }
            f.close();
        }
        if (sel >= n) sel = n - 1;
        if (sel < 0) sel = 0;
    };
    reload();

    Drawer g = { lcd_app_get_spr(), &tft };
    auto draw = [&]() {
        const Theme* th = g_th;
        if (sel < top) top = sel;
        if (sel > top + VISIBLE - 1) top = sel - (VISIBLE - 1);

        g.fillScreen(th->bg);
        ui_header(g, "Notes", (String(n) + " items").c_str());
        ui_footer(g, "TOP/DN move   CAM delete   MID back");

        if (n == 0) {
            g.setTextDatum(MC_DATUM);
            g.setTextFont(2);
            g.setTextSize(1);
            g.setTextColor(th->muted, th->bg);
            g.drawString("No notes yet", 160, 108);
            g.setTextFont(1);
            g.drawString("add them from the web page /notes", 160, 130);
            g.setTextDatum(TL_DATUM);
        } else {
            for (int i = 0; i < VISIBLE && top + i < n; ++i) {
                int idx = top + i;
                int y = 34 + i * 26;
                bool on = (idx == sel);
                uint16_t bg = on ? th->selBg : th->surface;
                g.fillRoundRect(8, y, 304, 24, 8, bg);
                if (on) {
                    g.drawRoundRect(8, y, 304, 24, 8, th->accent);
                    g.fillRoundRect(11, y + 7, 3, 10, 2, th->accent);
                }
                g.setTextDatum(TL_DATUM);
                g.setTextFont(1);
                g.setTextSize(1);
                g.setTextColor(th->muted, bg);
                g.drawString(String(idx + 1), 20, y + 8);

                String t = lines[idx];
                /* 320 宽 font1：约 6px/字符，去掉序号区后约 45 字符 */
                if (t.length() > 44) t = t.substring(0, 43) + "~";
                g.setTextColor(on ? th->text : th->text, bg);
                g.drawString(t, 40, y + 8);
            }
        }
        if (g.s) g.s->pushSprite(0, 0);
    };

    draw();
    while (!g_appExitReq) {
        bool changed = false;
        if (g_btTOPstate) {
            g_btTOPstate = 0;
            if (n > 0) { sel = (sel + n - 1) % n; changed = true; }
        }
        if (g_btDownstate) {
            g_btDownstate = 0;
            if (n > 0) { sel = (sel + 1) % n; changed = true; }
        }
        if (g_noteDeleteReq) {
            g_noteDeleteReq = false;
            if (n > 0 && sel < n) {
                SD.remove("/notes.txt");
                File f = SD.open("/notes.txt", FILE_WRITE);
                if (f) {
                    for (int i = 0; i < n; ++i)
                        if (i != sel) f.println(lines[i]);
                    f.close();
                }
                reload();
                changed = true;
            }
        }
        if (changed) draw();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    g_appExitReq = false;
    g_btTOPstate = g_btDownstate = 0;
}

/* =====================================================================
 * 占位/错误 App 主循环（MODE_ERROR：相机初始化失败等）
 * =================================================================== */
static void ui_placeholder_loop(const char* title, const char* sub)
{
    const Theme* th = g_th;
    tft.fillScreen(th->bg);
    tft.setTextSize(1);
    tft.setTextDatum(MC_DATUM);
    tft.setTextFont(2);
    tft.setTextColor(th->danger, th->bg);
    tft.drawString(title, 160, 90);
    tft.setTextFont(1);
    tft.setTextColor(th->text, th->bg);
    tft.drawString(sub, 160, 128);
    tft.setTextColor(th->muted, th->bg);
    tft.drawString("CAM / MID = back to desktop", 160, 190);
    tft.setTextDatum(TL_DATUM);

    while (!g_appExitReq) vTaskDelay(pdMS_TO_TICKS(20));
    g_appExitReq = false;
}

/* =====================================================================
 * 打开 App（keyTask CAM 单击上下文，8192 栈；相机初始化耗时数百 ms）
 * =================================================================== */
void ui_app_open(int idx)
{
    Serial.printf("[UI] 打开 App %d（%s）\n", idx,
                  idx >= 0 && idx < APP_COUNT ? s_appNames[idx] : "?");
    switch (idx) {
        case 0:   /* 相机：按需初始化摄像头 + 启动采集任务 */
            if (camera_app_init()) {
                rtos_restart_camera_task();
                g_appMode = MODE_CAMERA;
            } else {
                g_appMode = MODE_ERROR;   /* 错误占位页 */
            }
            break;
        case 1:
            g_appMode = MODE_GALLERY;
            break;
        case 2: g_appMode = MODE_SETTINGS; break;
        case 3: g_appMode = MODE_GAME;     break;
        case 4: g_appMode = MODE_NOTES;    break;
    }
}

/* =====================================================================
 * 串口截图（调试）：重绘当前桌面帧后，把 Sprite 像素 RLE 压缩 dump 出去。
 * 协议：ASCII 头 "IMG 320 240\n" → 二进制 RLE 包（颜色lo 颜色hi 步数lo 步数hi）
 *       ×N → "END\n"。上位机脚本还原成 PNG 供检查真实帧缓冲内容
 * =================================================================== */
void ui_app_serial_screenshot(int code)
{
    /* 模式：'S'(0) = 裸缓冲测试图案；'D'(1) = 完整桌面帧；
     * 'F'(2) = 仅白底；'T'(3) = 白底+瓷片（无图标/指示点）—— 分段定位 */
    static uint16_t* testBuf = nullptr;
    uint16_t* buf;
    if (code >= 1 && code <= 3) {
        draw_desktop_frame((float)g_appSel, code - 1);
        if (!s_deskSpr || !s_deskSpr->getPointer()) { Serial.println("IMGERR no-sprite"); return; }
        buf = (uint16_t*)s_deskSpr->getPointer();
    } else {
        if (!testBuf) testBuf = (uint16_t*)heap_caps_malloc(320 * 240 * 2, MALLOC_CAP_SPIRAM);
        if (!testBuf) { Serial.println("IMGERR alloc"); return; }
        for (int i = 0; i < 320 * 240; ++i) testBuf[i] = 0xFFFF;   /* 白底 */
        const uint16_t tc[5] = { 0xF800, 0xFFE0, 0x07E0, 0x001F, 0x780F };
        for (int k = 0; k < 5; ++k)
            for (int y = 100; y < 140; ++y)
                for (int x = 20 + k * 60; x < 60 + k * 60; ++x)
                    testBuf[y * 320 + x] = tc[k];
        buf = testBuf;
    }
    const int N = 320 * 240;
    /* 像素探针：读回关键坐标值，判断是"画错"还是"推屏/传输错"
     * 预期：p0=白FFF p80=黄FCA0 p160=天蓝33DE p240=橙FCA0 p318=白FFFF */
    Serial.printf("PROBE ptr=%p p0=%04X p80=%04X p160=%04X p240=%04X p318=%04X\n",
                  buf, buf[0], buf[120 * 320 + 80], buf[120 * 320 + 160],
                  buf[120 * 320 + 240], buf[120 * 320 + 318]);
    /* 直方图探针：统计整帧颜色分布 + 第一个非白像素位置 + 字节视角 FF 分布 */
    int cFF = 0, c00 = 0, cA0 = 0, cDE = 0, firstNon = -1;
    for (int i2 = 0; i2 < N; ++i2) {
        uint16_t v = buf[i2];
        if (v == 0xFFFF) { cFF++; continue; }
        if (firstNon < 0) firstNon = i2;
        if (v == 0x0000) c00++;
        else if (v == 0xFCA0) cA0++;
        else if (v == 0x33DE) cDE++;
    }
    uint8_t* pb = (uint8_t*)buf;
    int bFF1 = 0, bFF2 = 0;
    for (int i2 = 0; i2 < 76800; ++i2)        if (pb[i2] == 0xFF) bFF1++;
    for (int i2 = 76800; i2 < 153600; ++i2)   if (pb[i2] == 0xFF) bFF2++;
    Serial.printf("STAT ff=%d zero=%d fca0=%d 33de=%d firstNon=%d byteFF_lo=%d byteFF_hi=%d\n",
                  cFF, c00, cA0, cDE, firstNon, bFF1, bFF2);
    /* RLE 压缩进暂存缓冲，再 Base64 编码输出（纯文本协议，杜绝
     * 二进制流中随机出现 "END" 导致上位机提前截断/错位解码） */
    static uint8_t* rleBuf = nullptr;
    static uint32_t rleCap = 0;
    if (!rleBuf) { rleCap = 96 * 1024; rleBuf = (uint8_t*)heap_caps_malloc(rleCap, MALLOC_CAP_SPIRAM); }
    if (!rleBuf) { Serial.println("IMGERR alloc"); return; }
    uint32_t rc = 0;
    int i = 0;
    while (i < N && rc + 4 <= rleCap) {
        uint16_t c = buf[i];
        uint32_t run = 1;
        while (i + (int)run < N && buf[i + run] == c && run < 0xFFFF) run++;
        rleBuf[rc++] = c & 0xFF; rleBuf[rc++] = c >> 8;
        rleBuf[rc++] = run & 0xFF; rleBuf[rc++] = (run >> 8) & 0xFF;
        i += run;
    }
    Serial.printf("B64 %u\n", rc);
    static const char* tab = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (uint32_t p = 0; p < rc; p += 3) {
        uint32_t g = ((uint32_t)rleBuf[p] << 16) | ((p + 1 < rc ? rleBuf[p + 1] : 0) << 8) | (p + 2 < rc ? rleBuf[p + 2] : 0);
        char out[4] = { tab[(g >> 18) & 63], tab[(g >> 12) & 63], tab[(g >> 6) & 63], tab[g & 63] };
        Serial.write(out, 4);
        if ((p / 3) % 64 == 63) { Serial.print("\r\n"); Serial.flush(); delayMicroseconds(800); }
    }
    Serial.print("\r\nENDB64\n");
    Serial.flush();
}

/* =====================================================================
 * displayTask：App 调度器（config.h extern "C" 声明）
 *   [知识点1] 任务绑核由 rtos_app_init 统一完成（核1，优先级1）
 * =================================================================== */
void displayTask(void* arg)
{
    (void)arg;
    while (true) {
        switch (g_appMode) {
            case MODE_DESKTOP:
                ui_desktop_loop();             /* keyTask 打开 App 时才返回 */
                break;
            case MODE_CAMERA:
                ui_camera_loop();              /* 退出请求时释放摄像头后返回 */
                g_appMode = MODE_DESKTOP;
                break;
            case MODE_GALLERY:
                ui_gallery_loop();             /* 退出请求时返回 */
                g_appMode = MODE_DESKTOP;
                break;
            case MODE_SETTINGS:
                ui_settings_loop();
                g_appMode = MODE_DESKTOP;
                break;
            case MODE_GAME:
                game_app_loop();               /* 游戏选择页 + 三款游戏 */
                g_appMode = MODE_DESKTOP;
                break;
            case MODE_NOTES:
                ui_notes_loop();
                g_appMode = MODE_DESKTOP;
                break;
            default:                            /* MODE_ERROR 错误占位页 */
                ui_placeholder_loop("Error", "Camera init failed - any key back");
                g_appMode = MODE_DESKTOP;
                break;
        }
    }
}
