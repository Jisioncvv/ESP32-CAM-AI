/*
 * =====================================================================
 * ui_app.cpp  ——  桌面 + App 实现（Phase 2）
 * ---------------------------------------------------------------------
 * displayTask = App 调度器：按 g_appMode 分派到各 App 主循环
 *   桌面       ui_desktop_loop      横向轮播（左右循环，中间放大立体）
 *   相机 App   lcd_app.h ui_camera_loop（lcd_app.cpp，预览渲染）
 *   相册 App   lcd_app.h ui_gallery_loop（lcd_app.cpp，列表/全屏）
 *   设置 App   ui_settings_loop     RAM/PSRAM/CPU/照片数/SD/WiFi 系统信息
 *   游戏 App   ui_game_loop         俄罗斯方块（TOP/DOWN 移动 CAM 旋转 长按硬降，MID 退出）
 *   备忘 App   ui_notes_loop        列表查看 + CAM 删除；Web 端 /notes 添加
 * 图标为程序化绘制（TFT_eSPI 基本图形拼装），无需额外图片素材
 * 桌面风格：白底 + iOS 彩色圆角瓷砖 + Font2 精致字体（LOAD_FONT2）
 * =====================================================================
 */
#include "config.h"
#include "ui_app.h"
#include "lcd_app.h"
#include "camera_app.h"
#include "rtos_app.h"
#include "sd_app.h"
#include "web_app.h"
#include <SD.h>
#include <WiFi.h>
#include <time.h>

/* --------------------- 全局状态（ui_app.h extern） --------------------- */
volatile int  g_appMode    = MODE_DESKTOP;
volatile bool g_appExitReq = false;
volatile int  g_appSel     = 0;
volatile int8_t g_gameDirReq   = 0;      /* 游戏：输入请求 1=左转 2=右转 3=暂停 */
volatile bool g_noteDeleteReq = false;   /* 备忘：删除当前选中请求 */
volatile bool g_wifiUiOpenReq = false;   /* 设置 App：打开 WiFi 管理界面请求 */

/* --------------------- App 名称与瓷砖底色（iOS 风配色） ---------------- */
static const char* s_appNames[APP_COUNT] = {
    "Camera", "Album", "Setting", "Games", "Notes"
};
/* RGB565：蓝 #3478F6 / 橙 #FF9500 / 灰 #8E8E93 / 红 #FF3B30 / 黄 #FFCC00 */
static const uint16_t s_appColors[APP_COUNT] = {
    0x33DE, 0xFCA0, 0x8C72, 0xF9C6, 0xFE60
};

void ui_app_init(void)
{
    Serial.println("[UI] 桌面 + App 框架就绪（轮播桌面 / 设置 / 游戏 / 备忘）");
}

/* =====================================================================
 * 绘制目标包装：同一套调用既可画进相机 Sprite（整帧推屏，无闪烁），
 * 也可退化直绘 tft。C++11 无泛型 lambda，用结构体实现
 * [CSDN/资料确认] TFT_eSPI 防闪烁标准做法 = 整帧画进 TFT_eSprite 后
 * 一次 pushSprite；此前桌面只显示轮廓的根因 = 图标函数直绘 tft，
 * 被 pushSprite 整屏覆盖抹掉 —— 图标也必须画进 Sprite
 * =================================================================== */
struct Drawer {
    TFT_eSprite* s; TFT_eSPI* t;
    /* [根因修复] TFT_eSprite 没有重载 fillScreen()！继承版 fillScreen 内部调
     * fillRect(0,0,_width,_height)，而 sprite 对象的 _width/_height 是从
     * TFT_eSPI 构造函数继承的 TFT_WIDTH=240/TFT_HEIGHT=320，createSprite
     * 不会更新它们 → 白底只涂了左侧 240 列，右侧 80 列是 calloc 黑区
     * （串口 dump PNG 已实锤）。sprite 必须用 fillSprite()（按 _iwidth/
     * _yHeight=320x240 正确填充），只有直绘 tft 时才用 fillScreen() */
    void fillScreen(uint16_t c)   { if (s) s->fillSprite(c);   else t->fillScreen(c); }
    void fillRect(int x,int y,int w,int h,uint16_t c) { if (s) s->fillRect(x,y,w,h,c); else t->fillRect(x,y,w,h,c); }
    void fillRoundRect(int x,int y,int w,int h,int r,uint16_t c) { if (s) s->fillRoundRect(x,y,w,h,r,c); else t->fillRoundRect(x,y,w,h,r,c); }
    void drawRoundRect(int x,int y,int w,int h,int r,uint16_t c) { if (s) s->drawRoundRect(x,y,w,h,r,c); else t->drawRoundRect(x,y,w,h,r,c); }
    void fillTriangle(int x0,int y0,int x1,int y1,int x2,int y2,uint16_t c) { if (s) s->fillTriangle(x0,y0,x1,y1,x2,y2,c); else t->fillTriangle(x0,y0,x1,y1,x2,y2,c); }
    void fillCircle(int x,int y,int r,uint16_t c) { if (s) s->fillCircle(x,y,r,c); else t->fillCircle(x,y,r,c); }

    /* 文本接口包装（游戏/面板用，Sprite 与直绘同套调用） */
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

    /* 瓷片本体绘制（自动裁剪）：完整可见 → 圆角矩形；
     * 跨越屏幕边缘 → 只画屏幕内的矩形切片（无图标），实现自然滑出/滑入，
     * 杜绝越界写坏显存，也不会出现"右边缘消失、左边缘凭空出现"的瞬移 */
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

/* =====================================================================
 * 桌面图标绘制（程序化矢量图形，44x40 区域，左上角定位 x,y）
 * 全部通过 Drawer 画进当前帧缓冲（Sprite 或 tft）
 * =================================================================== */
static void drawIconCamera(Drawer& g, int x, int y, uint16_t fg)
{
    g.fillRect(x + 8, y + 2, 12, 6, fg);            /* 顶部取景凸起 */
    g.fillRoundRect(x, y + 6, 44, 30, 4, fg);       /* 机身 */
    g.fillCircle(x + 22, y + 21, 10, TFT_BLACK);    /* 镜头外圈（镂空） */
    g.fillCircle(x + 22, y + 21, 6, fg);            /* 镜头内芯 */
}
static void drawIconAlbum(Drawer& g, int x, int y, uint16_t fg)
{
    g.fillRoundRect(x, y + 2, 44, 34, 3, fg);       /* 相框 */
    g.fillTriangle(x + 6, y + 30, x + 18, y + 12, x + 30, y + 30, TFT_BLACK);  /* 山 */
    g.fillTriangle(x + 16, y + 30, x + 30, y + 16, x + 40, y + 30, TFT_BLACK); /* 前山 */
    g.fillCircle(x + 34, y + 10, 4, TFT_BLACK);     /* 太阳（镂空） */
    g.fillCircle(x + 34, y + 10, 3, fg);
}
static void drawIconSetting(Drawer& g, int x, int y, uint16_t fg)
{
    for (int i = 0; i < 8; ++i) {                   /* 8 个齿 */
        float a = i * 0.7854f;                      /* 45° */
        int dx = (int)(cosf(a) * 14), dy = (int)(sinf(a) * 14);
        g.fillRect(x + 19 + dx - 3, y + 18 + dy - 3, 7, 7, fg);
    }
    g.fillCircle(x + 22, y + 20, 12, fg);           /* 齿轮盘 */
    g.fillCircle(x + 22, y + 20, 6, TFT_BLACK);     /* 中孔 */
}
static void drawIconGame(Drawer& g, int x, int y, uint16_t fg)
{
    g.fillRoundRect(x, y + 6, 44, 26, 8, fg);       /* 手柄机身 */
    g.fillRect(x + 8, y + 13, 10, 4, TFT_BLACK);    /* 十字键横 */
    g.fillRect(x + 11, y + 10, 4, 10, TFT_BLACK);   /* 十字键竖 */
    g.fillCircle(x + 31, y + 13, 3, TFT_BLACK);     /* A 钮 */
    g.fillCircle(x + 37, y + 19, 3, TFT_BLACK);     /* B 钮 */
}
static void drawIconNotes(Drawer& g, int x, int y, uint16_t fg)
{
    g.fillRect(x + 4, y + 2, 36, 36, fg);           /* 纸张 */
    g.fillRect(x + 4, y + 2, 36, 7, TFT_NAVY);      /* 顶部色条 */
    for (int i = 0; i < 3; ++i)                     /* 三行字线 */
        g.fillRect(x + 9, y + 14 + i * 8, 26, 2, TFT_BLACK);
}

/* 图标分发表 */
typedef void (*IconDraw)(Drawer&, int, int, uint16_t);
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
    g.fillRect(196, 0, 124, 20, TFT_WHITE);     /* 状态区清底（重绘不残影） */

    /* ---- 时间 HH:MM ---- */
    struct tm tmv;
    char clockStr[8];
    if (getLocalTime(&tmv, 0))
        snprintf(clockStr, sizeof(clockStr), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    else
        strcpy(clockStr, "--:--");
    g.setTextFont(1);
    g.setTextSize(1);
    g.setTextColor(TFT_DARKGREY, TFT_WHITE);
    g.setTextDatum(TR_DATUM);
    g.drawString(clockStr, 314, 6);
    g.setTextDatum(TL_DATUM);

    /* ---- WiFi 4 格信号柱（RSSI 分档） ---- */
    int level = 0;
    if (WiFi.status() == WL_CONNECTED) {
        long r = WiFi.RSSI();
        level = (r > -55) ? 4 : (r > -65) ? 3 : (r > -75) ? 2 : 1;
    }
    int bx = 246, base = 17;
    for (int i = 0; i < 4; ++i) {
        int h = 3 + i * 3;                       /* 3/6/9/12 递增 */
        uint16_t c = (i < level) ? 0x33DE : 0xC618;
        g.fillRect(bx + i * 5, base - h, 3, h, c);
    }
}

static void draw_desktop_frame(float off, int dbgStage = 0)
{
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

    g.fillScreen(TFT_WHITE);                          /* 整屏白底（顶栏已按需求去掉） */
    if (dbgStage == 1) { if (spr) spr->pushSprite(0, 0); return; }   /* 调试：仅白底 */

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
        sy[i] = 120 - s / 2;
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
        if (selp) {
            if (full) g.fillRect(x + 4, y + 4, s, s, 0xC618);    /* 灰影 = 立体 */
            g.tileBody(x, y, s, s, 10, s_appColors[i]);
            if (full)
                for (int b2 = 0; b2 < 3; ++b2)                   /* 白色高光边框 */
                    g.drawRoundRect(x + b2, y + b2, s - 2 * b2, s - 2 * b2, 10 - b2, TFT_WHITE);
        } else {
            g.tileBody(x, y, s, s, 8, s_appColors[i]);
        }
        if (full && dbgStage == 0) s_icons[i](g, x + (s - 44) / 2, y + (s - 40) / 2, TFT_WHITE);
    }

    if (dbgStage == 2) { if (spr) spr->pushSprite(0, 0); return; }   /* 调试：白底+瓷片 */

    /* 底部指示点 */
    for (int i = 0; i < APP_COUNT; ++i) {
        int px = 160 - (APP_COUNT - 1) * 10 + i * 20;
        g.fillCircle(px, 222, i == center ? 4 : 2, i == center ? TFT_NAVY : 0xC618);
    }

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

/* 画单个按键（统一白底风格：选中=深蓝底白字，普通=浅灰底深字，Connect 绿） */
static void kb_draw_key(int k, bool sel)
{
    int x, y, w, h; kb_key_rect(k, x, y, w, h);
    uint16_t bg = sel ? TFT_NAVY
                : (k == 44) ? 0x03E0
                : (k == 43) ? 0xD69A : 0xE71C;
    uint16_t fg = sel ? TFT_WHITE
                : (k == 44) ? TFT_WHITE : TFT_NAVY;
    tft.fillRect(x + 1, y + 1, w - 2, h - 2, bg);
    tft.setTextColor(fg, bg);
    tft.setTextSize(1);
    tft.setTextDatum(MC_DATUM);
    tft.drawCentreString(kb_key_label(k), x + w / 2, y + h / 2, 2);
    tft.setTextDatum(TL_DATUM);
}

/* 密码行原位刷新（含光标 _，白底深字统一风格） */
static void kb_draw_pass(void)
{
    tft.fillRect(0, 24, 320, 20, TFT_WHITE);
    tft.drawFastHLine(0, 44, 320, 0xC618);
    tft.setTextSize(2);
    tft.setTextFont(2);
    tft.setTextColor(TFT_NAVY, TFT_WHITE);
    tft.setCursor(4, 26);
    String d = s_wPass;
    if (d.length() > 25) d = d.substring(d.length() - 25);   /* 长密码显示尾部 */
    tft.print(d + "_");
    tft.setTextSize(1);
}

/* 小键盘整屏（白底 + 深蓝顶栏，与桌面/设置统一） */
static void kb_draw_screen(void)
{
    tft.fillScreen(TFT_WHITE);
    tft.fillRect(0, 0, 320, 22, TFT_NAVY);
    tft.setTextSize(1);
    tft.setTextFont(2);
    tft.setTextColor(TFT_WHITE, TFT_NAVY);
    tft.setCursor(4, 4);
    String t = s_wCurSsid;
    if (t.length() > 24) t = t.substring(0, 24);
    tft.print("Password: " + t);
    tft.setTextFont(1);
    kb_draw_pass();
    for (int k = 0; k < 45; ++k) kb_draw_key(k, k == s_kbCur);
    tft.setTextColor(0x8410, TFT_WHITE);
    tft.setCursor(4, 216);
    tft.print("TOP/DN=move(hold=fast)  CAM=select  MID=cancel");
}

/* ---- 扫描/连接/结果 页面（统一白底 + 深蓝顶栏） ---- */
static void wifi_scan_wait_draw(void)
{
    tft.fillScreen(TFT_WHITE);
    tft.fillRect(0, 0, 320, 22, TFT_NAVY);
    tft.setTextFont(2);
    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE, TFT_NAVY);
    tft.setCursor(6, 4);
    tft.print("WiFi Setup");
    tft.setTextFont(1);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_NAVY, TFT_WHITE);
    tft.drawString("Scanning WiFi...", 160, 100);
    tft.setTextColor(0x8410, TFT_WHITE);
    tft.drawString("2.4GHz, about 3 seconds", 160, 128);
    tft.setTextDatum(TL_DATUM);
}

static void wifi_conn_draw(const String& ssid)
{
    tft.fillScreen(TFT_WHITE);
    tft.fillRect(0, 0, 320, 22, TFT_NAVY);
    tft.setTextFont(2);
    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE, TFT_NAVY);
    tft.setCursor(6, 4);
    tft.print("Connecting...");
    tft.setTextFont(1);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_NAVY, TFT_WHITE);
    String t = ssid;
    if (t.length() > 20) t = t.substring(0, 20);
    tft.drawString(t, 160, 104);
    tft.setTextColor(0x8410, TFT_WHITE);
    tft.drawString("please wait (up to 15s)", 160, 130);
    tft.setTextDatum(TL_DATUM);
}

static void wifi_result_draw(bool ok, const String& info)
{
    tft.fillScreen(TFT_WHITE);
    tft.fillRect(0, 0, 320, 22, ok ? 0x03E0 : 0xE71C);
    tft.setTextFont(2);
    tft.setTextSize(1);
    tft.setTextColor(ok ? TFT_WHITE : TFT_NAVY, ok ? 0x03E0 : 0xE71C);
    tft.setCursor(6, 4);
    tft.print(ok ? "Connected OK" : "Connect FAILED");
    tft.setTextFont(1);
    tft.setTextDatum(MC_DATUM);
    if (ok) {
        tft.setTextSize(2);
        tft.setTextColor(TFT_NAVY, TFT_WHITE);
        tft.drawString("IP: " + info, 160, 100);
        tft.setTextSize(1);
        tft.setTextColor(0x8410, TFT_WHITE);
        tft.drawString("Browser (same WiFi) -> this IP", 160, 132);
    } else {
        tft.setTextSize(1);
        tft.setTextColor(TFT_RED, TFT_WHITE);
        tft.drawString(info, 160, 108);
    }
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(0x8410, TFT_WHITE);
    tft.setCursor(4, 224);
    tft.print("CAM/MID = back");
}

/* WiFi 主菜单（白底 iOS 风，与设置页一致） */
static void wifi_menu_draw(int sel)
{
    tft.fillScreen(TFT_WHITE);
    tft.fillRect(0, 0, 320, 24, TFT_NAVY);
    tft.setTextFont(2);
    tft.setTextSize(1);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_NAVY);
    tft.drawString("WiFi Setup", 160, 12);
    tft.setTextDatum(TL_DATUM);

    bool apOn = (WiFi.getMode() & WIFI_MODE_AP);
    const char* items[3] = { "Scan & Connect", "Connect Saved",
                             apOn ? "Hotspot Mode : ON" : "Hotspot Mode : OFF" };
    for (int i = 0; i < 3; ++i) {
        int y = 42 + i * 36;
        if (i == sel) {
            tft.fillRect(8, y - 6, 304, 30, 0xC618);
            tft.drawRoundRect(8, y - 6, 304, 30, 6, TFT_NAVY);
        }
        tft.setTextFont(2);
        tft.setTextSize(1);
        tft.setTextColor(i == sel ? TFT_NAVY : 0x630C,
                         i == sel ? 0xC618 : TFT_WHITE);
        tft.setCursor(20, y);
        tft.print(items[i]);
    }
    tft.setTextColor(0x8410, TFT_WHITE);
    tft.setCursor(20, 42 + 3 * 36);
    tft.print("Status: " + web_app_wifi_status_text());
    tft.fillRect(0, 224, 320, 16, TFT_NAVY);
    tft.setTextColor(TFT_WHITE, TFT_NAVY);
    tft.setCursor(6, 227);
    tft.print("TOP/DN=move  CAM=select  MID=back");
}

/* 扫描结果列表（统一白底 + 深蓝顶栏，选中行浅灰高亮） */
static void wifi_list_draw(int sel)
{
    tft.fillScreen(TFT_WHITE);
    tft.fillRect(0, 0, 320, 22, TFT_NAVY);
    tft.setTextFont(2);
    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE, TFT_NAVY);
    tft.setCursor(6, 4);
    tft.print("WiFi List  " + String(s_wCount) + " found");
    tft.setTextFont(1);

    if (s_wCount == 0) {
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(TFT_DARKGREY, TFT_WHITE);
        tft.drawString("No networks", 160, 108);
        tft.setTextDatum(TL_DATUM);
        return;
    }
    if (sel < s_wListTop) s_wListTop = sel;
    if (sel > s_wListTop + 8) s_wListTop = sel - 8;
    if (s_wListTop < 0) s_wListTop = 0;
    for (int i = 0; i < 9 && s_wListTop + i < s_wCount; ++i) {
        int idx = s_wListTop + i;
        int y = 26 + i * 22;
        bool s = (idx == sel);
        if (s) tft.fillRect(4, y - 2, 312, 20, 0xC618);
        tft.setTextColor(s ? TFT_NAVY : 0x630C, s ? 0xC618 : TFT_WHITE);
        tft.setCursor(8, y + 2);
        String nm = s_wSsid[idx];
        if (nm.length() > 22) nm = nm.substring(0, 22);
        tft.print(nm.length() ? nm : String("(hidden)"));
        tft.setCursor(220, y + 2);
        tft.print(s_wOpen[idx] ? "OPEN" : "PWD");
        tft.setCursor(262, y + 2);
        tft.print(String(s_wRssi[idx]) + "dBm");
    }
    tft.fillRect(0, 226, 320, 14, TFT_NAVY);
    tft.setTextColor(TFT_WHITE, TFT_NAVY);
    tft.setCursor(6, 229);
    tft.print("TOP/DN=move  CAM=connect  MID=back");
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
                    kb_draw_pass();
                } else if (s_kbCur == 1) {               /* CLR 清空 */
                    s_wPass = "";
                    kb_draw_pass();
                } else if (s_kbCur == 2) {               /* 页面循环：数字→abc→ABC→SYM */
                    s_kbPage = (s_kbPage + 1) % 4;
                    kb_draw_screen();
                } else if (s_kbCur < 43) {               /* 字符键（密码上限 32） */
                    if (s_wPass.length() < 32) {
                        s_wPass += kb_key_label(s_kbCur);
                        kb_draw_pass();
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
                kb_draw_key(old, false);
                kb_draw_key(s_kbCur, true);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* =====================================================================
 * 设置 App：系统信息页（防闪烁：框架/标签只画一次，每秒只重写数值）
 * RAM / PSRAM / CPU 频率 / 运行时长 / 照片数 / SD / WiFi
 * [乱码根因修复] 开机自检页遗留 setTextSize(2)，首次进设置页时
 * 大字号叠行看似乱码（退出重进后因 update 已设回 1 而恢复）——
 * 每次绘制框架前显式复位 TextSize/TextFont/Datum，杜绝上游残留
 * =================================================================== */
static const int SET_ROW_Y = 34, SET_ROW_H = 22, SET_VAL_X = 308;
static const int SET_ROWS  = 8;                    /* RAM..WiFi 共 8 行可更新 */

static void draw_settings_frame(void)              /* 静态框架 + 标签（进 App 画一次） */
{
    tft.setTextDatum(TL_DATUM);                    /* 复位全部文本状态 */
    tft.setTextSize(1);
    tft.setTextFont(2);
    tft.fillScreen(TFT_WHITE);
    tft.fillRect(0, 0, 320, 24, TFT_NAVY);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_NAVY);
    tft.drawString("Settings", 160, 12);
    tft.setTextFont(2);
    tft.setTextDatum(TL_DATUM);

    static const char* labels[SET_ROWS] = {
        "RAM", "PSRAM", "CPU", "Uptime", "Photos", "SD", "WiFi", "FW"
    };
    for (int r = 0; r < SET_ROWS; ++r) {
        tft.setTextColor(0x630C, TFT_WHITE);       /* 深灰标签（白底不闪） */
        tft.setCursor(12, SET_ROW_Y + r * SET_ROW_H);
        tft.print(labels[r]);
    }
    /* 底部操作提示（CAM 进 WiFi 管理） */
    tft.fillRect(0, 224, 320, 16, TFT_NAVY);
    tft.setTextColor(TFT_WHITE, TFT_NAVY);
    tft.setCursor(6, 227);
    tft.print("CAM=WiFi Setup   MID=exit");
}

static void update_settings_values(void)           /* 数值原位刷新（不带 fillScreen） */
{
    uint32_t heapTot = ESP.getHeapSize(),  heapFree = ESP.getFreeHeap();
    uint32_t psTot   = ESP.getPsramSize(), psFree   = ESP.getFreePsram();
    uint32_t up      = millis() / 1000;

    tft.setTextDatum(TR_DATUM);
    tft.setTextSize(1);
    tft.setTextPadding(140);           /* [残影修复] 新值比旧值短时残留字符 */
    auto val = [&](int r, String v) {
        tft.setTextColor(TFT_BLACK, TFT_WHITE);    /* 不透明白底：覆盖旧值无残影 */
        tft.drawString(v, SET_VAL_X, SET_ROW_Y + r * SET_ROW_H);
    };
    val(0, String(heapFree / 1024) + " / " + heapTot / 1024 + " KB");
    val(1, String(psFree / 1024) + " / " + psTot / 1024 + " KB");
    val(2, String(ESP.getCpuFreqMHz()) + " MHz");
    char upt[20];
    snprintf(upt, sizeof(upt), "%lu:%02lu:%02lu",
             (unsigned long)(up / 3600), (unsigned long)((up / 60) % 60), (unsigned long)(up % 60));
    val(3, String(upt));

    /* 照片数扫 SD 较慢，5 秒才扫一次 */
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
    val(4, String(photos));
    val(5, sd_app_is_ready() ? "Ready" : "Missing");
    val(6, web_app_wifi_status_text());            /* STA IP / AP 192.168.4.1 / OFF */
    val(7, "v2.4 wifi-kb");
    tft.setTextPadding(0);
    tft.setTextDatum(TL_DATUM);
}

static void ui_settings_loop(void)
{
    draw_settings_frame();
    update_settings_values();
    uint32_t lastUpd = millis();
    while (!g_appExitReq) {
        if (g_wifiUiOpenReq) {                     /* CAM 单击：进 WiFi 管理子界面 */
            g_wifiUiOpenReq = false;
            ui_wifi_loop();
            g_btTOPstate = g_btDownstate = 0;      /* 清子界面残留按键标志 */
            draw_settings_frame();                 /* 整页重画（被子界面覆盖过） */
            update_settings_values();
            lastUpd = millis();
            continue;
        }
        uint32_t now = millis();
        if (now - lastUpd >= 1000) {   /* 1 秒刷新数值（不重画整页 → 不闪） */
            update_settings_values();
            lastUpd = now;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    g_appExitReq = false;
    g_btTOPstate = g_btDownstate = 0;
}

/* =====================================================================
 * 游戏 App：俄罗斯方块（Tetris）
 *   TOP = 左移  DOWN = 右移  CAM 单击 = 旋转（带踢墙）  CAM 长按 = 硬降
 *   MID 单击 = 退出（keyTask 通用分支）  Game Over 后任意键重开
 * 按键语义 g_gameDirReq：1=左移 2=右移 3=旋转 4=硬降
 * [贪吃蛇教训] 棋盘等大状态一律 static —— 贪吃蛇 2.4KB 栈数组曾撑爆
 * 任务栈导致"一启动就崩"；static 局部变量在 BSS 段，无栈压力
 * 渲染复用相机离屏 Sprite 整帧绘制（防闪烁），Sprite 不可用时退化直绘
 * =================================================================== */
#define TET_COLS 10
#define TET_ROWS 20
#define TET_CELL 12                 /* 棋盘 120x240 屏幕左侧，右侧信息面板 */

/* 7 种方块 × 4 旋转态 × 4 个 (dx,dy)：各旋转态在 4x4 盒内的格子坐标 */
static const uint8_t s_tetShapes[7][4][8] = {
    { /* I */
        {0,1,1,1,2,1,3,1}, {2,0,2,1,2,2,2,3},
        {0,2,1,2,2,2,3,2}, {1,0,1,1,1,2,1,3} },
    { /* O */
        {1,0,2,0,1,1,2,1}, {1,0,2,0,1,1,2,1},
        {1,0,2,0,1,1,2,1}, {1,0,2,0,1,1,2,1} },
    { /* T */
        {1,0,0,1,1,1,2,1}, {1,0,1,1,2,1,1,2},
        {0,1,1,1,2,1,1,2}, {1,0,0,1,1,1,1,2} },
    { /* S */
        {1,0,2,0,0,1,1,1}, {1,0,1,1,2,1,2,2},
        {1,1,2,1,0,2,1,2}, {0,0,0,1,1,1,1,2} },
    { /* Z */
        {0,0,1,0,1,1,2,1}, {2,0,1,1,2,1,1,2},
        {0,1,1,1,1,2,2,2}, {1,0,0,1,1,1,0,2} },
    { /* J */
        {0,0,0,1,1,1,2,1}, {1,0,2,0,1,1,1,2},
        {0,1,1,1,2,1,2,2}, {1,0,1,1,0,2,1,2} },
    { /* L */
        {2,0,0,1,1,1,2,1}, {1,0,1,1,1,2,2,2},
        {0,1,1,1,2,1,0,2}, {0,0,1,0,1,1,1,2} },
};

/* 方块配色（I 青 O 黄 T 紫 S 绿 Z 红 J 蓝 L 橙） */
static const uint16_t s_tetColors[7] = {
    0x07FF, 0xFFE0, 0xF81F, 0x07E0, 0xF800, 0x001F, 0xFD20
};

static uint8_t s_tetBoard[TET_ROWS][TET_COLS];   /* 0=空 1..7=方块色号 */

/* 碰撞检测：方块 t/r 态放到 (px,py) 是否越界或压到已有格子 */
static bool tet_collide(int t, int r, int px, int py)
{
    const uint8_t* c = s_tetShapes[t][r];
    for (int i = 0; i < 4; ++i) {
        int bx = px + c[i * 2], by = py + c[i * 2 + 1];
        if (bx < 0 || bx >= TET_COLS || by >= TET_ROWS) return true;
        if (by >= 0 && s_tetBoard[by][bx]) return true;
    }
    return false;
}

/* 落定：写进棋盘（by<0 顶部外的格子忽略） */
static void tet_lock(int t, int r, int px, int py)
{
    const uint8_t* c = s_tetShapes[t][r];
    for (int i = 0; i < 4; ++i) {
        int bx = px + c[i * 2], by = py + c[i * 2 + 1];
        if (by >= 0 && by < TET_ROWS && bx >= 0 && bx < TET_COLS)
            s_tetBoard[by][bx] = (uint8_t)(t + 1);
    }
}

/* 消行：满行下移，返回消除行数 */
static int tet_clear_lines(void)
{
    int cleared = 0;
    for (int y = TET_ROWS - 1; y >= 0; --y) {
        bool full = true;
        for (int x = 0; x < TET_COLS; ++x)
            if (!s_tetBoard[y][x]) { full = false; break; }
        if (full) {
            ++cleared;
            for (int yy = y; yy > 0; --yy)
                for (int x = 0; x < TET_COLS; ++x) s_tetBoard[yy][x] = s_tetBoard[yy - 1][x];
            for (int x = 0; x < TET_COLS; ++x) s_tetBoard[0][x] = 0;
            ++y;   /* 当行重查（上面的行落下来后可能也满） */
        }
    }
    return cleared;
}

static void ui_game_loop(void)
{
    TFT_eSprite* spr = lcd_app_get_spr();
    Drawer g = { spr, &tft };
    const int BX = 24, BY = 0;           /* 棋盘屏幕原点（120 宽，右侧面板 160..316） */

    int curT = 0, curR = 0, curX = 3, curY = 0, nextT = 0;
    int score = 0, lines = 0, over = 0;
    uint32_t tickMs = 500, lastTick = millis();
    auto spawn = [&](void) {
        curT = nextT;
        nextT = random(0, 7);
        curR = 0; curX = 3; curY = 0;
        if (tet_collide(curT, curR, curX, curY)) over = 1;   /* 出生即重叠 = 顶死 */
        tickMs = (500 - lines * 10 > 120) ? (500 - lines * 10) : 120;
    };
    /* 开局：清盘 + 发两块 */
    for (int y = 0; y < TET_ROWS; ++y)
        for (int x = 0; x < TET_COLS; ++x) s_tetBoard[y][x] = 0;
    nextT = random(0, 7);
    score = lines = over = 0;
    spawn();
    g_gameDirReq = 0;

    while (!g_appExitReq) {
        /* ---- 输入：每帧最多消费一个请求 ---- */
        if (g_gameDirReq) {
            int8_t req = g_gameDirReq; g_gameDirReq = 0;
            if (over) {                              /* Game Over 任意键重开 */
                for (int y = 0; y < TET_ROWS; ++y)
                    for (int x = 0; x < TET_COLS; ++x) s_tetBoard[y][x] = 0;
                score = lines = 0; tickMs = 500; over = 0;
                nextT = random(0, 7);
                spawn();
                lastTick = millis();
                continue;
            }
            if (req == 1 && !tet_collide(curT, curR, curX - 1, curY)) --curX;
            else if (req == 2 && !tet_collide(curT, curR, curX + 1, curY)) ++curX;
            else if (req == 3) {                     /* 旋转 + 简单踢墙（原位/左1/右1） */
                int nr = (curR + 1) & 3;
                if (!tet_collide(curT, nr, curX, curY))          curR = nr;
                else if (!tet_collide(curT, nr, curX - 1, curY)) { curR = nr; --curX; }
                else if (!tet_collide(curT, nr, curX + 1, curY)) { curR = nr; ++curX; }
            }
            else if (req == 4) {                     /* 硬降：直落到底并锁定 */
                while (!tet_collide(curT, curR, curX, curY + 1)) ++curY;
                tet_lock(curT, curR, curX, curY);
                lines += tet_clear_lines();
                score += 5;
                spawn();
            }
        }

        /* ---- 重力节拍（消行加速） ---- */
        if (!over && millis() - lastTick >= tickMs) {
            lastTick = millis();
            if (!tet_collide(curT, curR, curX, curY + 1)) {
                ++curY;
            } else {                                 /* 落定 → 消行 → 下一块 */
                tet_lock(curT, curR, curX, curY);
                int c = tet_clear_lines();
                lines += c;
                score += (c == 1) ? 10 : (c == 2) ? 30 : (c == 3) ? 60 : (c == 4) ? 100 : 0;
                spawn();
            }
        }

        /* ---- 渲染：左棋盘 + 右面板（Next/得分/行数） ---- */
        g.fillScreen(TFT_WHITE);
        g.fillRect(BX - 2, BY - 2, TET_COLS * TET_CELL + 4, TET_ROWS * TET_CELL + 4, 0x8C72);  /* 边框 */
        for (int y = 0; y < TET_ROWS; ++y)
            for (int x = 0; x < TET_COLS; ++x)
                if (s_tetBoard[y][x])
                    g.fillRect(BX + x * TET_CELL + 1, BY + y * TET_CELL + 1,
                               TET_CELL - 2, TET_CELL - 2, s_tetColors[s_tetBoard[y][x] - 1]);
        if (!over) {
            const uint8_t* c = s_tetShapes[curT][curR];
            for (int i = 0; i < 4; ++i) {
                int bx = curX + c[i * 2], by = curY + c[i * 2 + 1];
                if (by >= 0)
                    g.fillRect(BX + bx * TET_CELL + 1, BY + by * TET_CELL + 1,
                               TET_CELL - 2, TET_CELL - 2, s_tetColors[curT]);
            }
        }
        /* 右面板 */
        g.fillRoundRect(160, 10, 148, 220, 8, 0xCE79);
        g.setTextColor(TFT_BLACK, 0xCE79);
        g.setTextDatum(TL_DATUM);
        g.setTextFont(2);
        g.drawCentreString("Next", 234, 22, 2);
        {   /* Next 预览（4x4 盒居中缩放 2 格 = 8px） */
            const uint8_t* c = s_tetShapes[nextT][0];
            int mx = 0, Mx = 3, my = 0, My = 0;
            for (int i = 0; i < 4; ++i) {
                if (c[i * 2] < mx) mx = c[i * 2];
                if (c[i * 2] > Mx) Mx = c[i * 2];
                if (c[i * 2 + 1] < my) my = c[i * 2 + 1];
                if (c[i * 2 + 1] > My) My = c[i * 2 + 1];
            }
            int ox = 234 - ((Mx - mx + 1) * 8) / 2 - mx * 8;
            int oy = 60 - ((My - my + 1) * 8) / 2 - my * 8;
            for (int i = 0; i < 4; ++i)
                g.fillRect(ox + c[i * 2] * 8, oy + c[i * 2 + 1] * 8, 7, 7, s_tetColors[nextT]);
        }
        g.drawCentreString("Score " + String(score), 234, 110, 2);
        g.drawCentreString("Lines " + String(lines), 234, 132, 2);
        g.drawCentreString("T/D move", 234, 162, 1);
        g.drawCentreString("CAM rot", 234, 174, 1);
        g.drawCentreString("hold CAM drop", 234, 186, 1);
        if (over) {
            g.fillRoundRect(60, 92, 200, 64, 6, TFT_NAVY);
            g.setTextDatum(MC_DATUM);
            g.setTextColor(TFT_WHITE, TFT_NAVY);
            g.drawCentreString("Game Over", 160, 110, 2);
            g.drawCentreString("key = retry  MID = exit", 160, 132, 1);
            g.setTextDatum(TL_DATUM);
        }
        if (spr) spr->pushSprite(0, 0);
        vTaskDelay(pdMS_TO_TICKS(15));   /* 输入响应粒度（实际下落由 tickMs 控制） */
    }
    g_appExitReq = false;
    g_gameDirReq = 0;
}

/* =====================================================================
 * 备忘 App：/notes.txt 每行一条（Web 端 /notes 页面添加）
 *   TOP/DOWN 移动选中；CAM 删除当前选中；MID 退出
 * =================================================================== */
static void ui_notes_loop(void)
{
    static const int MAX_NOTES = 50;
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

    auto draw = [&]() {
        if (sel < top) top = sel;
        if (sel > top + 7) top = sel - 7;
        tft.fillScreen(TFT_WHITE);
        tft.setTextSize(1);                    /* 复位上游遗留的字号状态 */
        tft.setTextDatum(TL_DATUM);
        tft.fillRect(0, 0, 320, 24, TFT_NAVY);
        tft.setTextFont(2);
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(TFT_WHITE, TFT_NAVY);
        tft.drawString("Notes (" + String(n) + ")", 160, 12);
        tft.setTextFont(1);
        tft.setTextDatum(TL_DATUM);

        if (n == 0) {
            tft.setTextDatum(MC_DATUM);
            tft.setTextColor(0x8410, TFT_WHITE);
            tft.drawString("no notes", 160, 110);
            tft.drawString("add via WiFi page /notes", 160, 130);
            tft.setTextDatum(TL_DATUM);
        } else {
            for (int i = 0; i < 8 && top + i < n; ++i) {
                int idx = top + i;
                int y = 30 + i * 22;
                if (idx == sel) tft.fillRect(4, y - 2, 312, 20, 0xC618);  /* 选中高亮 */
                tft.setTextColor(idx == sel ? TFT_NAVY : 0x630C,
                                 idx == sel ? 0xC618 : TFT_WHITE);
                tft.setCursor(10, y);
                tft.print(idx + 1);
                tft.print(". ");
                String t = lines[idx];
                if (t.length() > 42) t = t.substring(0, 42);
                tft.print(t);
            }
        }
        tft.fillRect(0, 226, 320, 14, TFT_NAVY);
        tft.setTextColor(TFT_WHITE, TFT_NAVY);
        tft.setCursor(6, 229);
        tft.print("UP/DN=move  CAM=del  MID=back");
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
    tft.fillScreen(TFT_WHITE);
    tft.setTextSize(1);
    tft.setTextDatum(MC_DATUM);
    tft.setTextFont(2);
    tft.setTextColor(TFT_NAVY, TFT_WHITE);
    tft.drawString(title, 160, 90);
    tft.setTextFont(1);
    tft.setTextColor(0x630C, TFT_WHITE);
    tft.drawString(sub, 160, 128);
    tft.setTextColor(0x8410, TFT_WHITE);
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
                ui_game_loop();
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
