/*
 * =====================================================================
 * lcd_app.cpp  ——  LCD 屏幕模块实现（渲染层 + 相机/相册 App 主循环）
 * ---------------------------------------------------------------------
 * App 化结构（Phase 1 桌面 + App 框架）：
 *   ui_camera_loop  相机 App：RGB565 帧 → Sprite 合成 → 推屏（退出即断电）
 *   ui_gallery_loop 相册 App：列表菜单 ↔ 全屏查看（调度器在 ui_app.cpp）
 * 本项目性能优化（相对官方逐帧 createSprite/deleteSprite）：
 *   1. 静态 TFT_eSprite 合成整帧后再推屏 —— 文字不再闪烁
 *   2. 相册列表用纯文本行（不走缩略图），翻页秒刷；
 *      全屏查看用 TJpgDec 缩放解码 + PSRAM 缓存，回看秒开
 * 硬件实测结论（本项目排障记录）：
 *   - ST7789 @ SPI 27MHz + IPS 反色开（platformio.ini -DTFT_INVERSION_ON）
 *   - TFT_eSPI 的 initDMA() 在 S3+core2.0.17 上会使 SPI 总线 wedge，
 *     故全程同步推屏，不启用 DMA
 * =====================================================================
 */
#include "config.h"
#include "lcd_app.h"
#include "theme.h"
#include "camera_app.h"
#include "sd_app.h"
#include "ui_app.h"
#include <TFT_eSPI.h>
#include <TJpg_Decoder.h>
#include <SD.h>
#include <mbedtls/base64.h>
/* JPEGDEC 已移除：预览改用相机 RGB565 直出（官方 InitCameraConfig 画质方案） */

TFT_eSPI tft = TFT_eSPI();
bool     g_lcdReady = false;
float    g_lcdFps   = 0;

/* --------------------- 官方同款显示状态 --------------------- */
static bool showSavingPopup = false;          /* 官方 showSavingPopup */
static unsigned long lastMillis = 0;
static int frames = 0;
/* 诊断剖析（帧率/推屏/等帧耗时，供串口定位瓶颈） */
static uint32_t s_pushUs = 0;
static uint32_t s_waitUs = 0;

/* --------------------- 静态合成 Sprite（只建一次，内存稳定） --------------------- */
static TFT_eSprite* s_spr      = nullptr;
static bool         s_sprReady = false;

/* --------------------- 相册状态（文件夹式文本列表） --------------------- */
#define MAX_PHOTOS   64

static String s_names[MAX_PHOTOS];   /* 文件名（不含路径） */
static bool   s_small[MAX_PHOTOS];   /* 对应小图 photo_Ns.jpg 是否存在 */
static int    s_count    = 0;        /* 照片总数 */
static int    s_cur      = 0;        /* 当前选中序号 0..s_count-1 */
static uint32_t s_listGen = 0;       /* 列表代数：buildPhotoList 每次重建 +1，
                                        九宫格缩略图缓存据此判断是否失效 */

/* 相册全屏缓存（PSRAM 150KB）：看过的照片秒开（25ms 推屏） */
static uint16_t* s_cacheBuf = nullptr;   /* 320x240x2 */
static int       s_cacheIdx = -1;        /* 缓存对应的照片序号 */
static bool      s_caching  = false;     /* 当前正在解码并写缓存 */
/* 预解码缓冲（PSRAM 150KB）：空闲时后台解码相邻照片，翻页秒开 */
static uint16_t* s_nextBuf  = nullptr;
static int       s_nextIdx  = -1;        /* 已预解码完成的照片序号（-1=无） */
static int       s_prefetchTarget = -1;  /* 待预解码的照片序号 */
static bool      s_prefetchBusy = false; /* 后台解码进行中 */
static bool      s_prefetchReq  = false; /* 有相邻照片待后台解码 */
static bool      s_listDirty    = true;  /* 列表过期标志：拍照后需重扫 SD */

/* 预览帧缓冲：直接复用 Sprite 显存（RGB565 无损直写，无 JPEG 解码环节） */
static uint16_t* s_previewBuf = nullptr; /* 320x240x2 */

/* =====================================================================
 * TJpgDec 回调：两种工作模式
 *   1) 默认：官方 tft_output 1:1 上屏，全屏查看时同步写入 PSRAM 缓存
 *   2) s_decodeToRam=true：仅 memcpy 到目标缓冲（后台预解码相邻照片，不动屏幕）
 * 注意：TJpgDec 输出小端 RGB565，上屏前 setSwapBytes(true)
 * =================================================================== */
static bool s_decodeToRam = false;       /* true=解码进内存（预解码模式） */
static uint16_t* s_ramTarget = nullptr;  /* 预解码目标缓冲 */

static bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap)
{
    /* [越界保护] 解码块必须逐行裁剪后再写缓冲：x+w 超出 320 时，旧版
     * 直接 memcpy(w*h*2) 会把这一块的右边几列挤到下一行的行首，画面
     * 表现为"斜切 + 一条条横向错行"，缓存里还会攒下一整片黑缝。 */
    if (s_decodeToRam && s_ramTarget) {
        /* 预解码模式：写内存缓冲后立即返回，不碰屏幕 */
        for (int row = 0; row < h; ++row) {
            int dy = y + row;
            if (dy >= 240) break;
            int cw = w;
            if (x + cw > 320) cw = 320 - x;
            if (cw <= 0) continue;
            memcpy(s_ramTarget + dy * 320 + x, bitmap + row * w, cw * 2);
        }
        return true;
    }
    if (y >= tft.height()) return false;
    tft.pushImage(x, y, w, h, bitmap);
    if (s_caching && s_cacheBuf) {
        for (int row = 0; row < h; ++row) {
            int dy = y + row;
            if (dy >= 240) break;
            int cw = w;
            if (x + cw > 320) cw = 320 - x;
            if (cw <= 0) continue;
            memcpy(s_cacheBuf + dy * 320 + x, bitmap + row * w, cw * 2);
        }
    }
    return true;
}

/* 预览不再走 JPEGDEC：相机输出 RGB565（官方 InitCameraConfig），帧数据
 * 大端序直接 memcpy 进 Sprite 帧缓冲，与官方 sprite.pushImage(fb->buf) 等价。 */

static void draw_splash(void);   /* 前置声明：开机 splash 画面在下方定义 */

void lcd_app_init(void)
{
    Serial.println("[LCD] 初始化中...");
    /* 背光先关，避免初始化过程出现花屏闪烁 */
    pinMode(PIN_LCD_BL, OUTPUT);
    digitalWrite(PIN_LCD_BL, LOW);

    tft.begin();
    tft.setRotation(LCD_ROTATION);   /* 横屏 320x240 */
    tft.fillScreen(TFT_BLACK);

    /* 相册全屏缓存（PSRAM 150KB）：看过的照片秒开 */
    s_cacheBuf = (uint16_t*)heap_caps_malloc(320 * 240 * 2, MALLOC_CAP_SPIRAM);
    Serial.printf("[LCD] 相册缓存 %s\n", s_cacheBuf ? "OK" : "分配失败（回看走全解码）");
    /* 相册预解码缓冲（PSRAM 150KB）：后台解码相邻照片，翻页秒开 */
    s_nextBuf = (uint16_t*)heap_caps_malloc(320 * 240 * 2, MALLOC_CAP_SPIRAM);
    Serial.printf("[LCD] 相册预解码缓冲 %s\n", s_nextBuf ? "OK" : "分配失败（仅当前图缓存）");
    /* 预览：用 Sprite 帧缓冲作为 RGB565 帧目标（复用，零额外拷贝，文字零闪烁）。
     * 相机 RGB565 帧 memcpy 进 Sprite 显存 → drawString 叠加文字 → pushSprite 推屏 */
    s_spr = new TFT_eSprite(&tft);
    if (s_spr) {
        s_spr->setColorDepth(16);
        s_previewBuf = (uint16_t*)s_spr->createSprite(320, 240);
        if (s_previewBuf) {
            s_spr->setSwapBytes(false);   /* 相机 RGB565 帧为大端序，直接推不换字节 */
            s_sprReady = true;
            Serial.println("[LCD] 预览 Sprite 复用帧缓冲（解码直写，文字零闪烁）");
        } else {
            delete s_spr;
            s_spr = nullptr;
            s_previewBuf = (uint16_t*)heap_caps_malloc(320 * 240 * 2, MALLOC_CAP_INTERNAL);
            Serial.printf("[LCD] Sprite 失败，退化直推 %s\n", s_previewBuf ? "OK" : "也失败");
        }
    }

    /* 开机 splash：企鹅 + 标题，趁背光未开画完，点亮即完整显示 */
    draw_splash();

    digitalWrite(PIN_LCD_BL, HIGH);  /* 开背光 */
    g_lcdReady = true;
    Serial.printf("[LCD] 初始化成功 ST7789 %dx%d 旋转=%d\n",
                  tft.width(), tft.height(), LCD_ROTATION);
}

/* =====================================================================
 * 开机画面：企鹅图标 + 标题 + 进度条（配色全部跟随当前主题）
 * ---------------------------------------------------------------------
 * [本次改进] 旧版是硬编码白底 + TFT_NAVY 标题：切到深色主题后，开机
 *   一瞬间整屏惨白，和桌面/App 的深色风格完全断裂，像换了台机器。
 *   现在页面底色/文字/强调色都取 g_th，并把原本干等的 3 秒做成
 *   进度条动画，开机过程有反馈。
 * 图标由 assets/penguin.jpg 转换（assets/convert_penguin.ps1 可重新生成）
 * =================================================================== */
#include "splash_penguin.h"

#define SPLASH_TITLE "ESP32-CAM"          /* 改这里即可换名字 */
#define SPLASH_SUB   "FreeRTOS Camera System"

/* 布局常量：splash 与自检页共用同一套几何，切换时画面不跳变 */
#define SPLASH_ICON_X   112
#define SPLASH_ICON_Y    24
#define SPLASH_TITLE_Y  150
#define SPLASH_INFO_Y   176
#define SPLASH_BAR_X     50
#define SPLASH_BAR_Y    200
#define SPLASH_BAR_W    220
#define SPLASH_BAR_H      6
#define SPLASH_HINT_Y   214

/* 绘制企鹅图标（左上角定位） */
static void draw_penguin_img(int x, int y)
{
    tft.setSwapBytes(true);
    tft.pushImage(x, y, PENGUIN_W, PENGUIN_H, gPenguin);
    tft.setSwapBytes(false);
}

/* 图标底板：企鹅素材是白底 JPEG 转出来的，直接贴到深色底上会多出一个
 * 白方块。统一垫一张"应用图标"式白色圆角卡，两套主题下都像正规图标 */
static void draw_splash_icon(void)
{
    const Theme* th = g_th;
    int x = SPLASH_ICON_X - 7, y = SPLASH_ICON_Y - 7;
    tft.fillRoundRect(x, y, PENGUIN_W + 14, PENGUIN_H + 14, 20, TFT_WHITE);
    tft.drawRoundRect(x, y, PENGUIN_W + 14, PENGUIN_H + 14, 20, th->line);
    draw_penguin_img(SPLASH_ICON_X, SPLASH_ICON_Y);
}

/* 进度条：pct = 0..100（轨道恒画满，填充随进度增长） */
static void draw_splash_bar(int pct)
{
    const Theme* th = g_th;
    tft.fillRoundRect(SPLASH_BAR_X, SPLASH_BAR_Y, SPLASH_BAR_W, SPLASH_BAR_H,
                      SPLASH_BAR_H / 2, th->surface2);
    int w = SPLASH_BAR_W * pct / 100;
    if (w >= SPLASH_BAR_H)
        tft.fillRoundRect(SPLASH_BAR_X, SPLASH_BAR_Y, w, SPLASH_BAR_H,
                          SPLASH_BAR_H / 2, th->accent);
}

/* 页面骨架：主题底色 + 图标 + 标题（splash/自检页共用） */
static void draw_splash_base(void)
{
    const Theme* th = g_th;
    tft.fillScreen(th->bg);
    draw_splash_icon();

    tft.setTextDatum(MC_DATUM);
    tft.setTextFont(4);
    tft.setTextSize(1);
    tft.setTextColor(th->text, th->bg);
    tft.drawString(SPLASH_TITLE, 160, SPLASH_TITLE_Y);
    tft.setTextDatum(TL_DATUM);
}

/* 一行小字提示（主题弱化色） */
static void draw_splash_hint(const char* txt)
{
    const Theme* th = g_th;
    tft.setTextDatum(MC_DATUM);
    tft.setTextFont(1);
    tft.setTextSize(1);
    tft.setTextColor(th->muted, th->bg);
    tft.drawString(txt, 160, SPLASH_HINT_Y);
    tft.setTextDatum(TL_DATUM);
}

/* 开机 splash：屏幕点亮即显示，摄像头/SD 初始化期间不再黑屏 */
static void draw_splash(void)
{
    const Theme* th = g_th;
    draw_splash_base();
    tft.setTextDatum(MC_DATUM);
    tft.setTextFont(2);
    tft.setTextSize(1);
    tft.setTextColor(th->muted, th->bg);
    tft.drawString(SPLASH_SUB, 160, SPLASH_INFO_Y);
    tft.setTextDatum(TL_DATUM);
    draw_splash_bar(0);
    draw_splash_hint("Booting...");
}

/* 自检状态小标签：底色取主题 OSD，文字用各模块自己的状态色 */
static void draw_boot_chip(int x, int y, const char* txt, uint16_t fg)
{
    Drawer d{ nullptr, &tft };
    ui_osd_chip(d, x, y, txt, fg);
}

void lcd_app_show_boot_screen(bool camOk, bool sdOk)
{
    if (!g_lcdReady) return;
    const Theme* th = g_th;

    draw_splash_base();

    /* 中间一行状态标签：CAM / SD / WiFi（替代旧版"左图标 + 右列表"的分栏） */
    char sdTxt[16];
    snprintf(sdTxt, sizeof(sdTxt), "SD %s", sdOk ? "OK" : "FAIL");
    const char* wifiTxt = "WiFi OFF";            /* [按需 WiFi] 开机不开热点 */
    (void)camOk;                                  /* 相机按需启动，只看启动方式 */

    int w1 = ui_osd_width("CAM APP");
    int w2 = ui_osd_width(sdTxt);
    int w3 = ui_osd_width(wifiTxt);
    int gap = 8;
    int x = (320 - (w1 + w2 + w3 + gap * 2)) / 2;
    draw_boot_chip(x,             SPLASH_INFO_Y - 3, "CAM APP", th->accent);
    draw_boot_chip(x + w1 + gap,  SPLASH_INFO_Y - 3, sdTxt,    sdOk ? th->ok : th->danger);
    draw_boot_chip(x + w1 + w2 + gap * 2, SPLASH_INFO_Y - 3, wifiTxt, th->muted);

    draw_splash_bar(0);
    draw_splash_hint("Initializing system...");
    Serial.println("[LCD] 开机自检：主题化 splash + 状态标签已显示");
}

/* 自检停留期间跑进度条动画：把原本 3 秒的"黑等"变成有反馈的加载过程
 * （分步填充，每步只改 220x6 的一小块，不影响后续任务启动时间） */
void lcd_app_hold_boot_screen(int ms)
{
    if (!g_lcdReady) { vTaskDelay(pdMS_TO_TICKS(ms)); return; }
    const int steps = 48;
    int stepMs = ms / steps;
    if (stepMs < 1) stepMs = 1;
    for (int i = 1; i <= steps; ++i) {
        draw_splash_bar(i * 100 / steps);
        vTaskDelay(pdMS_TO_TICKS(stepMs));
    }
    draw_splash_hint("Starting...");
}

void lcd_app_set_saving_popup(bool on)
{
    showSavingPopup = on;
}

/* 复用相机离屏 Sprite（320x240 全帧缓冲）：游戏等 App 整帧渲染防闪烁；
 * 返回 nullptr 表示 Sprite 不可用（PSRAM 分配失败），调用方回退直绘 */
TFT_eSprite* lcd_app_get_spr(void)
{
    return s_sprReady ? s_spr : nullptr;
}

/* 拍照/删除后调用：下次进相册重扫 SD 卡（列表缓存刷新） */
void lcd_app_mark_gallery_dirty(void)
{
    s_listDirty = true;
}

/* =====================================================================
 * 相册：扫描 SD 卡照片列表（photo_N.jpg，按序号排序）
 * =================================================================== */
static void buildPhotoList(void)
{
    s_count = 0;
    File root = SD.open("/");
    if (!root) return;
    while (true) {
        File f = root.openNextFile();
        if (!f) break;
        if (!f.isDirectory()) {
            String name = String(f.name());
            /* 跳过相册专用小图 photo_Ns.jpg（由大图文件名 + 小图标记管理） */
            bool isSmall = name.startsWith("photo_") && name.endsWith("s.jpg") &&
                           !name.endsWith("ss.jpg");
            if (isSmall) continue;
            if (name.startsWith("photo_") && name.endsWith(".jpg") &&
                s_count < MAX_PHOTOS) {
                /* 按序号插入排序（photo_2 排在 photo_10 前面） */
                int idx = name.substring(6, name.indexOf('.')).toInt();
                int pos = s_count;
                while (pos > 0) {
                    int prevIdx = s_names[pos - 1].substring(6,
                                    s_names[pos - 1].indexOf('.')).toInt();
                    if (prevIdx > idx) {
                        s_names[pos] = s_names[pos - 1];
                        s_small[pos] = s_small[pos - 1];
                        --pos;
                    } else break;
                }
                s_names[pos] = name;
                /* 探测同名小图 photo_Ns.jpg（相册秒开的关键标记） */
                String smallName = name;
                smallName.replace(".jpg", "s.jpg");
                s_small[pos] = SD.exists("/" + smallName);
                s_count++;
            }
        }
        f.close();
    }
    root.close();
    s_listGen++;   /* 九宫格缩略图缓存失效标记 */
    /* 列表变了（拍照新增/删除）→ 照片序号与文件名的对应关系可能整体错位，
     * 全屏缓存与预解码缓存必须作废，否则会"点开 A 显示 B" */
    s_cacheIdx = -1;
    s_nextIdx  = -1;
    s_prefetchReq = false;
}

/* 相册取图路径：有小图用小图（秒开），没有则回退大图全解码 */
static String galleryImagePath(int listIdx)
{
    if (s_small[listIdx]) {
        String p = "/" + s_names[listIdx];
        p.replace(".jpg", "s.jpg");
        return p;
    }
    return "/" + s_names[listIdx];
}

/* 自适应解码缩放：按 JPEG 实际宽度选 TJpgDec scale（仅支持 1/2/4/8）
 * 输出恒为 320x240：VGA 小图 scale=2（源 2x2 超采样，画质更细腻），
 * QVGA 小图 scale=1，QSXGA 大图 scale=8 */
static uint8_t pickJpgScale(const String& path)
{
    uint16_t w = 0, h = 0;
    if (TJpgDec.getSdJpgSize(&w, &h, path) == JDR_OK && w > 0) {
        if (w <= 340)  return 1;
        if (w <= 680)  return 2;
        if (w <= 1360) return 4;
    }
    return 8;   /* QSXGA 2560 或尺寸读取失败 */
}

/* 全屏解码几何：先按实际尺寸选缩放，再把解码结果在 320x240 里居中。
 * [黑带修复 2026-11-26] 旧版固定从 (0,0) 解码：HD 拍照档 1280x720 按
 * scale=4 只解出 320x180，屏幕下面 60px 就只剩 fillScreen 的纯黑，看着
 * 像"一条黑带/黑格子"压在照片底下。现在居中，四周留白统一用主题底色。 */
static void fullJpgGeom(const String& path, uint8_t& sc,
                        int& ox, int& oy, int& ow, int& oh)
{
    sc = pickJpgScale(path);
    uint16_t sw = 0, sh = 0;
    if (TJpgDec.getSdJpgSize(&sw, &sh, path) != JDR_OK || sw == 0 || sh == 0) {
        ox = oy = 0; ow = 320; oh = 240;
        return;
    }
    ow = (int)sw / sc;
    oh = (int)sh / sc;
    if (ow <= 0) ow = 320;
    if (oh <= 0) oh = 240;
    if (ow > 320) ow = 320;         /* 极宽的图会超出屏幕：钉死在屏幕内 */
    if (oh > 240) oh = 240;
    ox = (320 - ow) / 2;
    oy = (240 - oh) / 2;
}

/* 整屏缓冲铺色（缓存/预解码缓冲用同一颜色，回贴才和屏幕逐点一致） */
static void frame_fill(uint16_t* buf, uint16_t c)
{
    if (!buf) return;
    for (int i = 0; i < 320 * 240; ++i) buf[i] = c;
}

/* =====================================================================
 * 相册：九宫格缩略图页（3x3 每页 9 张，替代文本列表）
 *   链路：photo_Ns.jpg（VGA 小图）→ pickJpgScale → TJpgDec 解码到
 *   320x240 暂存（复用 s_nextBuf）→ 最近邻降采样 102x68 存进 PSRAM
 *   网格缓存（9 张一块 ~124KB）
 *   缓存命中（页号+列表代数相同）→ 页内移动只刷新选中框，瞬时响应；
 *   跨页/拍照/删除才重新解码（~0.3~1s，页顶显示 Loading 提示）
 * =================================================================== */
#define GRID_COLS 3
#define GRID_ROWS 3
#define GRID_CNT  (GRID_COLS * GRID_ROWS)
#define TH_W      102     /* 缩略图尺寸 */
#define TH_H      60
#define CELL_W    106     /* 单元格尺寸（含边距） */
#define CELL_H    63
#define GRID_TOP  32      /* 网格起始 y（30px 顶栏下方） */

static uint16_t* s_gridBuf  = nullptr;   /* PSRAM：9 张缩略图连续存储 */
static int       s_gridTop  = -1;       /* 缓存窗口首格对应的列表序号（3 的倍数，-1=无） */
static uint32_t  s_gridGen  = 0;        /* 缓存对应列表代数 */
static int       s_gridSelDrawn = -1;   /* 已画选中项（局部刷新用） */
/* 待解码位图：bit k 对应窗口内第 k 格（0..8）。
 * 快速滚动（长按连发）时行滚动只搬缓存、不解码，新进的那一行先显示
 * 浅灰占位；主循环在"本轮没有按键输入"时调用 grid_flush_pending 补齐。
 * 位图必须与缓存同步移位，否则会漏格或重复解码。 */
static uint16_t  s_gridPendMask = 0;

/* 区域平均降采样：320x240 暂存 → TH_WxTH_H（按实际解码后的 ow x oh 映射）
 * [观感修复] 旧版最近邻只取单像素：暗光照片的高增益噪点被抽样成彩色麻点，
 * 在 102x68 缩略图上像"马赛克/花屏"。改为按源区域做 3x3~4x4 平均，
 * 噪点被平滑掉，缩略图观感干净且不再出现错行/黑带 */
static void grid_decimate(uint16_t* dst, const uint16_t* src, int ow, int oh)
{
    if (ow <= 0 || ow > 320) ow = 320;
    if (oh <= 0 || oh > 240) oh = 240;
    for (int y = 0; y < TH_H; ++y) {
        int sy0 = y * oh / TH_H;
        int sy1 = (y + 1) * oh / TH_H;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        uint16_t* dr = dst + y * TH_W;
        for (int x = 0; x < TH_W; ++x) {
            int sx0 = x * ow / TH_W;
            int sx1 = (x + 1) * ow / TH_W;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            uint32_t rs = 0, gs = 0, bs = 0, cnt = 0;
            for (int sy = sy0; sy < sy1; ++sy) {
                const uint16_t* sr = src + sy * 320;
                for (int sx = sx0; sx < sx1; ++sx) {
                    uint16_t c = sr[sx];
                    rs += (c >> 11) & 0x1F;
                    gs += (c >> 5)  & 0x3F;
                    bs += c & 0x1F;
                    ++cnt;
                }
            }
            uint16_t r = (uint16_t)(rs / cnt);
            uint16_t g = (uint16_t)(gs / cnt);
            uint16_t b = (uint16_t)(bs / cnt);
            dr[x] = (uint16_t)((r << 11) | (g << 5) | b);
        }
    }
}

/* JPEG 结尾校验：残图（缺 FF D9）解码会吐马赛克/蓝噪，必须提前拦截。
 * [误判修复 2026-11-26] 旧版只看"最后 2 字节 == FF D9"：SD 卡按簇写入、
 * 或者拍照小图还在后台落盘时，文件尾部会带一段 0x00 填充，EOI 明明在，
 * 却被判成残图 → 这一格缩略图直接变成纯黑方块（相册里"黑色格子"）。
 * 现在回扫尾部 1KB 找最后一个 FF D9：JPEG 熵编码里 0xFF 后面必定跟 0x00，
 * 所以 FF D9 只可能是真 EOI，不会误判。 */
static bool jpeg_tail_ok(const String& path)
{
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    size_t sz = f.size();
    bool ok = false;
    if (sz >= 4) {
        uint8_t tail[512];
        size_t win  = (sz < sizeof(tail)) ? sz : sizeof(tail);
        size_t base = sz - win;
        if (f.seek(base)) {
            size_t rd = f.read(tail, win);
            for (int i = (int)rd - 2; i >= 0; --i) {
                if (tail[i] == 0xFF && tail[i + 1] == 0xD9) { ok = true; break; }
            }
        }
    }
    f.close();
    return ok;
}

/* 缩略图占位块：解码失败/被跳过时用主题灰块，而不是 memset(0) 的纯黑。
 * [黑色网格根因之一] 深色主题下，一格纯黑和"黑屏"没区别，几格连起来
 * 就是一片黑色网格盖在相册上；改成 surface + 一条灰色短横线，一眼能看懂
 * 是"这张没有预览"而不是"屏幕坏了" */
static void grid_placeholder(uint16_t* dst)
{
    const Theme* th = g_th;
    for (int i = 0; i < TH_W * TH_H; ++i) dst[i] = th->surface;
    int y = TH_H / 2;
    for (int x = TH_W / 2 - 12; x < TH_W / 2 + 12; ++x) dst[y * TH_W + x] = th->line;
}

/* 解码一张图 → 102x68 缩略图写进 dst；返回是否成功 */
static bool grid_decode_one(const String& path, uint16_t* dst)
{
    memset(s_nextBuf, 0, 320 * 240 * 2);   /* 解码填不满的区域保持黑，杜绝残影 */
    if (!jpeg_tail_ok(path)) {
        Serial.printf("[GAL] 缩略图 JPEG 残缺，跳过: %s\n", path.c_str());
        return false;
    }
    /* 实际解码输出尺寸 = 源尺寸 / scale，降采样按它映射，杜绝错行/黑带 */
    uint16_t sw = 0, sh = 0;
    uint8_t  sc = 8;
    if (TJpgDec.getSdJpgSize(&sw, &sh, path) == JDR_OK && sw > 0) {
        sc = (sw <= 340) ? 1 : (sw <= 680) ? 2 : (sw <= 1360) ? 4 : 8;
    }
    int ow = sw ? (int)sw / sc : 320;
    int oh = sh ? (int)sh / sc : 240;
    uint32_t t0 = millis();
    TJpgDec.setJpgScale(sc);
    TJpgDec.setCallback(tft_output);
    s_decodeToRam = true;
    s_ramTarget   = s_nextBuf;
    bool ok = (TJpgDec.drawSdJpg(0, 0, path) == JDR_OK);
    s_decodeToRam = false;
    s_ramTarget   = nullptr;
    /* [诊断] 解码路径/缩放/耗时：定位缩略图马赛克是文件问题还是解码读卡问题 */
    Serial.printf("[GAL] thumb %s sc=%d %s ok=%d %ums\n", path.c_str(), sc,
                  sw ? "size-ok" : "size-FAIL", (int)ok, millis() - t0);
    if (ok) grid_decimate(dst, s_nextBuf, ow, oh);
    return ok;
}

/* 解码窗口内第 k 格（k=0..8）的缩略图进缓存（失败画占位块，绝不留黑格） */
static void grid_load_thumb(int topIdx, int k)
{
    int listIdx = topIdx + k;
    uint16_t* dst = s_gridBuf + k * TH_W * TH_H;
    if (listIdx >= s_count || !s_nextBuf) { grid_placeholder(dst); return; }

    /* 优先小图（秒开）；小图残缺/解码失败 → 回退大图，避免黑格与马赛克 */
    if (grid_decode_one(galleryImagePath(listIdx), dst)) return;
    if (s_small[listIdx] && grid_decode_one("/" + s_names[listIdx], dst)) return;
    grid_placeholder(dst);
}

/* 待解码占位（快速滚动时先铺，松手后由 grid_flush_pending 补齐）
 * 小圆点用强调色：深色主题下 surface2 本来就偏暗，若再用灰点，一片格子
 * 看起来就是"黑色方块"；强调色一眼能读成"正在解码" */
static void grid_draw_pending(int cx, int cy)
{
    const Theme* th = g_th;
    tft.fillRect(cx, cy, TH_W, TH_H, th->surface2);
    tft.setTextDatum(MC_DATUM);
    tft.setTextFont(1);
    tft.setTextSize(1);
    tft.setTextColor(th->accent, th->surface2);
    tft.drawString("...", cx + TH_W / 2, cy + TH_H / 2);
    tft.setTextDatum(TL_DATUM);
}

/* 选中框（设备端统一风格：蓝色圆角细框，与桌面/设置同色） */
static void grid_draw_sel(int listIdx, uint16_t c)
{
    if (listIdx < 0 || listIdx >= s_count || s_gridTop < 0) return;
    int k = listIdx - s_gridTop;
    if (k < 0 || k >= GRID_CNT) return;
    int x = (k % GRID_COLS) * CELL_W;
    int y = GRID_TOP + (k / GRID_COLS) * CELL_H;
    tft.drawRoundRect(x, y, TH_W + 2, TH_H + 2, 4, c);
    tft.drawRoundRect(x + 1, y + 1, TH_W, TH_H, 4, c);
}

/* 清除选中框：回贴缩略图恢复像素 + 外圈涂白（绝不残影） */
static void grid_clear_sel(int listIdx)
{
    if (listIdx < 0 || listIdx >= s_count || !s_gridBuf || s_gridTop < 0) return;
    int k = listIdx - s_gridTop;
    if (k < 0 || k >= GRID_CNT) return;
    int x = (k % GRID_COLS) * CELL_W + 1;
    int y = GRID_TOP + (k / GRID_COLS) * CELL_H + 1;
    if (s_gridPendMask & (1u << k)) {
        /* 该格还在等解码：回贴旧数据会闪出错位照片，保持浅灰占位即可 */
        tft.fillRect(x, y, TH_W, TH_H, g_th->surface2);
        tft.drawRect(x - 1, y - 1, TH_W + 2, TH_H + 2, g_th->bg);
        return;
    }
    tft.setSwapBytes(true);
    tft.pushImage(x, y, TH_W, TH_H, s_gridBuf + k * TH_W * TH_H);
    tft.setSwapBytes(false);
    tft.drawRect(x - 1, y - 1, TH_W + 2, TH_H + 2, g_th->bg);
}

/* 顶栏 / 底栏（与 theme.h 的 ui_header/ui_footer 同一几何：30px / 20px） */
static void grid_draw_topbar(void)
{
    tft.fillRect(0, 0, 320, 30, g_th->header);
    tft.drawFastHLine(0, 29, 320, g_th->line);
    tft.setTextDatum(TL_DATUM);
    tft.setTextFont(1);        /* 钉死 8px 字模，防止继承上个 App 的大字号 */
    tft.setTextSize(1);
    tft.setTextColor(g_th->text, g_th->header);
    tft.setCursor(12, 11);
    tft.print("Photos");
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(g_th->muted, g_th->header);
    char hdr[24];
    if (s_count > 0) snprintf(hdr, sizeof(hdr), "%d / %d", s_cur + 1, s_count);
    else             snprintf(hdr, sizeof(hdr), "0 / 0");
    tft.drawString(hdr, 308, 11);
    tft.setTextDatum(TL_DATUM);
}

static void grid_draw_bottombar(void)
{
    tft.fillRect(0, 220, 320, 20, g_th->header);
    tft.drawFastHLine(0, 220, 320, g_th->line);
    tft.setTextDatum(MC_DATUM);
    tft.setTextFont(1);
    tft.setTextSize(1);
    tft.setTextColor(g_th->muted, g_th->header);
    tft.drawString("TOP/DN move   CAM open   MID back", 160, 230);
    tft.setTextDatum(TL_DATUM);
}

/* =====================================================================
 * 补齐延迟解码的格子（快速滚动时滚动只搬缓存、不解码）
 *   s_gridPendMask 的每一位对应窗口内第 k 格，置位 = 数据还没解码。
 *   主循环确认"本轮没有任何按键输入"后才调用 —— 长按连发期间一直被
 *   跳过，所以按住可以连续滑，松手后再集中补齐，不会被解码阻塞。
 * =================================================================== */
static void grid_flush_pending(void)
{
    if (!s_gridPendMask || s_gridTop < 0 || !s_gridBuf || !s_nextBuf) return;
    uint16_t mask = s_gridPendMask;
    s_gridPendMask = 0;
    int done = 0;
    uint32_t t0 = millis();
    tft.setSwapBytes(true);
    for (int k = 0; k < GRID_CNT; ++k) {
        if (!(mask & (1u << k))) continue;
        int cx = (k % GRID_COLS) * CELL_W + 1;
        int cy = GRID_TOP + (k / GRID_COLS) * CELL_H + 1;
        if (s_gridTop + k >= s_count) {
            tft.fillRect(cx, cy, TH_W, TH_H, g_th->bg);
            continue;
        }
        grid_load_thumb(s_gridTop, k);
        tft.pushImage(cx, cy, TH_W, TH_H, s_gridBuf + k * TH_W * TH_H);
        ++done;
    }
    tft.setSwapBytes(false);
    /* s_nextBuf 被借用为解码暂存：作废全屏预解码缓存，防止全屏误命中 */
    s_nextIdx = -1;
    s_prefetchReq    = false;
    s_prefetchBusy   = false;
    s_prefetchTarget = -1;
    Serial.printf("[GAL] 补齐 %d 格缩略图（%ums）\n", done,
                  (unsigned)(millis() - t0));
}

/* 九宫格绘制（连续滚动窗口）：
 *   窗口 = s_gridTop 起的 9 格；选中项越界时窗口按"整行"滚动一行，
 *   重叠的 6 张缩略图直接复用（memmove），只解码新进的一行 → 流畅不闪 */
static void galleryGridPage(void)
{
    /* ---- 1. 计算窗口位置（按行对齐，让 s_cur 落在可视 3 行内） ---- */
    int curRow = (s_count > 0) ? s_cur / GRID_COLS : 0;
    int topRow = (s_gridTop >= 0) ? s_gridTop / GRID_COLS
                                  : (curRow > GRID_ROWS - 1 ? curRow - (GRID_ROWS - 1) : 0);
    if (curRow < topRow) topRow = curRow;
    if (curRow > topRow + GRID_ROWS - 1) topRow = curRow - (GRID_ROWS - 1);
    /* 末尾不留整行空白：窗口顶不超过 (总行数 - 3) */
    int maxTopRow = (s_count > 0) ? ((s_count - 1) / GRID_COLS) - (GRID_ROWS - 1) : 0;
    if (maxTopRow < 0) maxTopRow = 0;
    if (topRow > maxTopRow) topRow = maxTopRow;
    int newTop = topRow * GRID_COLS;

    bool genOk  = (s_gridGen == s_listGen);
    bool hasBuf = (s_gridBuf != nullptr);
    if (!hasBuf && !s_gridBuf)
        s_gridBuf = (uint16_t*)heap_caps_malloc(TH_W * TH_H * GRID_CNT * 2,
                                                MALLOC_CAP_SPIRAM);
    hasBuf = (s_gridBuf != nullptr);
    int  delta = (s_gridTop >= 0) ? (newTop - s_gridTop) : 999;

    bool fullRedraw = (!hasBuf || !genOk || s_gridTop < 0 ||
                       delta > GRID_COLS || delta < -GRID_COLS);

    if (hasBuf && !fullRedraw && delta != 0) {
        /* ---- 2a. 整行滚动：复用重叠 6 张，只解码新进的那一行 ----
         * [花屏根因修复 2026-11-26] 旧写法
         *   memmove(s_gridBuf, s_gridBuf + 3 * cellBytes, 6 * cellBytes)
         * 把"字节数"当成了元素偏移：s_gridBuf 是 uint16_t*，指针 +N 前进
         * N 个 2 字节元素，于是 s_gridBuf + 3*13872 实际跨了 6 格。结果：
         *   目的 0..5 格 ← 源 6..11 格（第 9~11 格已越出 9 格缓冲，读到
         *   相邻 PSRAM 分配 = 未初始化垃圾）。
         * 现象完全对应：新解的那一行被复制到第 0 行 → 显示正常；中间行
         * （第 3~5 格）装的是越界垃圾 → 彩色噪点花屏。上移分支更糟，目的
         * 地址写到第 6~11 格，会直接踩坏缓冲区后面的内存。
         * 现统一换算成 uint8_t 字节基址 + 行字节数，彻底杜绝类型混用。 */
        const int rows      = delta / GRID_COLS;                       /* ±1 */
        const int cellBytes = TH_W * TH_H * (int)sizeof(uint16_t);
        const int rowBytes  = GRID_COLS * cellBytes;                   /* 一整行 3 格 */
        uint8_t*  base      = (uint8_t*)s_gridBuf;
        /* 位图跟着数据一起移位；新进的那一行标记为"待解码"，先占位 */
        if (rows > 0) {                            /* 下移一行：第 1~2 行 → 第 0~1 行 */
            memmove(base, base + rowBytes, (GRID_ROWS - 1) * rowBytes);
            s_gridPendMask >>= GRID_COLS;
            s_gridPendMask |= (uint16_t)(0x7u << (GRID_CNT - GRID_COLS));
        } else {                                   /* 上移一行：第 0~1 行 → 第 1~2 行 */
            memmove(base + rowBytes, base, (GRID_ROWS - 1) * rowBytes);
            s_gridPendMask <<= GRID_COLS;
            s_gridPendMask |= 0x7u;
        }
        s_gridPendMask &= (uint16_t)((1u << GRID_CNT) - 1);   /* 只保留窗口内 9 位 */
        s_gridTop = newTop;
        s_gridSelDrawn = -1;
        /* s_nextBuf 被借用为缩略图解码暂存：作废预解码缓存，防止全屏误命中 */
        s_nextIdx = -1;
        s_prefetchReq    = false;
        s_prefetchBusy   = false;
        s_prefetchTarget = -1;
        /* 只重贴 9 张缩略图（不解码），几乎瞬时 */
        /* [残影修复] 选中框描边画在格子间的白槽里，pushImage 只覆盖图片区域，
         * 必须先整块涂白，否则旧选中框会留在原格边缘形成错位残影 */
        tft.fillRect(0, 30, 320, 190, g_th->bg);   /* 顶栏下沿到页脚之间的整块 */
        tft.setSwapBytes(true);
        for (int k = 0; k < GRID_CNT; ++k) {
            int cx = (k % GRID_COLS) * CELL_W + 1;
            int cy = GRID_TOP + (k / GRID_COLS) * CELL_H + 1;
            if (newTop + k >= s_count)
                tft.fillRect(cx, cy, TH_W, TH_H, g_th->bg);
            else if (s_gridPendMask & (1u << k))
                grid_draw_pending(cx, cy);          /* 待解码：灰色占位块（非纯黑） */
            else
                tft.pushImage(cx, cy, TH_W, TH_H, s_gridBuf + k * TH_W * TH_H);
        }
        tft.setSwapBytes(false);
        grid_draw_topbar();
        grid_draw_sel(s_cur, g_th->accent);
        grid_draw_bottombar();
        s_gridSelDrawn = s_cur;
        return;
    }

    if (!fullRedraw) {
        if (s_gridSelDrawn != s_cur && hasBuf) {
            /* ---- 2b. 窗口内移动：只刷选中框 + 顶栏（秒刷） ---- */
            grid_clear_sel(s_gridSelDrawn);
            grid_draw_sel(s_cur, g_th->accent);
            grid_draw_topbar();
            grid_draw_bottombar();
            s_gridSelDrawn = s_cur;
        }
        return;
    }

    /* ---- 3. 整页重建（首次进入 / 大跳 / 列表变化） ---- */
    if (hasBuf) {
        tft.fillScreen(g_th->bg);
        grid_draw_topbar();
        tft.setTextDatum(TL_DATUM);
        tft.setTextFont(1);
        tft.setTextSize(1);
        tft.setTextColor(g_th->muted, g_th->bg);
        tft.setCursor(12, 60);
        tft.print("Loading thumbnails...");
        for (int k = 0; k < GRID_CNT; ++k) grid_load_thumb(newTop, k);
        s_nextIdx = -1;                /* s_nextBuf 被借用为解码暂存：复位预解码 */
        s_prefetchReq    = false;
        s_prefetchBusy   = false;
        s_prefetchTarget = -1;
        s_gridTop = newTop;
        s_gridGen = s_listGen;
        s_gridPendMask = 0;            /* 整页已同步解码完毕，撤销待解码位图 */
    }

    tft.fillScreen(g_th->bg);
    grid_draw_topbar();
    if (s_count == 0) {
        tft.setTextDatum(MC_DATUM);
        tft.setTextFont(2);
        tft.setTextSize(1);
        tft.setTextColor(g_th->muted, g_th->bg);
        tft.drawString("No photos", 160, 108);
        tft.setTextFont(1);
        tft.drawString("Take one with CAM in preview", 160, 132);
        tft.setTextDatum(TL_DATUM);
    } else if (hasBuf) {
        tft.setSwapBytes(true);        /* TJpgDec 输出小端 RGB565 */
        for (int k = 0; k < GRID_CNT; ++k) {
            int cx = (k % GRID_COLS) * CELL_W + 1;
            int cy = GRID_TOP + (k / GRID_COLS) * CELL_H + 1;
            if (newTop + k < s_count)
                tft.pushImage(cx, cy, TH_W, TH_H, s_gridBuf + k * TH_W * TH_H);
        }
        tft.setSwapBytes(false);
    } else {
        for (int k = 0; k < GRID_CNT && newTop + k < s_count; ++k) {
            int cx = (k % GRID_COLS) * CELL_W + 1;
            int cy = GRID_TOP + (k / GRID_COLS) * CELL_H + 1;
            tft.fillRect(cx, cy, TH_W, TH_H, g_th->surface2);
            tft.setTextDatum(MC_DATUM);
            tft.setTextFont(2);
            tft.setTextSize(1);
            tft.setTextColor(g_th->muted, g_th->surface2);
            /* MC_DATUM + drawString：drawCentreString 是 TC_DATUM，会把
             * y 当顶边，文字整块往下坠，这里统一改掉 */
            tft.drawString(s_names[newTop + k], cx + TH_W / 2, cy + TH_H / 2);
            tft.setTextDatum(TL_DATUM);
        }
    }
    grid_draw_sel(s_cur, g_th->accent);
    grid_draw_bottombar();
    s_gridSelDrawn = s_cur;
}

/* =====================================================================
 * 诊断命令（串口 gstat / gshot）：验证九宫格缓存内容与列表映射
 *   gstat —— 打印窗口状态 + 每格对应文件 + 缓存内容 CRC（判断重复/错位）
 *   gshot —— 把 s_gridBuf 的 9 格按屏幕布局拼成 320x240，RLE+Base64 回传
 * =================================================================== */
void lcd_app_gallery_debug_dump(void)
{
    Serial.printf("[GDBG] count=%d cur=%d top=%d selDrawn=%d view=%d buf=%d gen=%u/%u\n",
                  s_count, s_cur, s_gridTop, s_gridSelDrawn, (int)g_galleryView,
                  s_gridBuf ? 1 : 0, (unsigned)s_gridGen, (unsigned)s_listGen);
    if (!s_gridBuf) return;
    for (int k = 0; k < GRID_CNT; ++k) {
        int idx = s_gridTop + k;
        if (idx < 0 || idx >= s_count) {
            Serial.printf("[GDBG] k=%d idx=%d (空)\n", k, idx);
            continue;
        }
        uint32_t h = 2166136261u;
        const uint16_t* p = s_gridBuf + k * TH_W * TH_H;
        for (int i = 0; i < TH_W * TH_H; ++i) { h ^= p[i]; h *= 16777619u; }
        Serial.printf("[GDBG] k=%d idx=%d name=%s small=%d hash=%08X\n",
                      k, idx, s_names[idx].c_str(), (int)s_small[idx], (unsigned)h);
    }
}

void lcd_app_gallery_debug_shot(void)
{
    if (!s_gridBuf) { Serial.println("[GDBG] no grid buf"); return; }
    const int W = 320, H = 240;
    static uint16_t* out = nullptr;
    static uint8_t*  rle = nullptr;
    if (!out) out = (uint16_t*)heap_caps_malloc(W * H * 2, MALLOC_CAP_SPIRAM);
    if (!rle) rle = (uint8_t*)heap_caps_malloc(W * H * 4 + 16, MALLOC_CAP_SPIRAM);
    if (!out || !rle) { Serial.println("[GDBG] alloc fail"); return; }

    for (int i = 0; i < W * H; ++i) out[i] = 0xFFFF;      /* 白底 */
    for (int k = 0; k < GRID_CNT; ++k) {
        int idx = s_gridTop + k;
        if (idx < 0 || idx >= s_count) continue;
        int cx = (k % GRID_COLS) * CELL_W + 1;
        int cy = GRID_TOP + (k / GRID_COLS) * CELL_H + 1;
        for (int y = 0; y < TH_H; ++y)
            memcpy(out + (cy + y) * W + cx, s_gridBuf + k * TH_W * TH_H + y * TH_W,
                   TH_W * 2);
    }

    /* RLE：16bit 颜色 + 16bit 重复数（1/4 抽样输出 80x60，USB 串口传输更快） */
    const int W2 = 80, H2 = 60;
    size_t rl = 0;
    int i = 0;
    while (i < W2 * H2) {
        int sx = (i % W2) * 4, sy = (i / W2) * 4;
        uint16_t c = out[sy * W + sx];
        int run = 1;
        while (i + run < W2 * H2 && run < 60000) {
            int nx = ((i + run) % W2) * 4, ny = ((i + run) / W2) * 4;
            if (out[ny * W + nx] != c) break;
            ++run;
        }
        rle[rl++] = c & 0xFF; rle[rl++] = c >> 8;
        rle[rl++] = run & 0xFF; rle[rl++] = run >> 8;
        i += run;
    }

    Serial.printf("B64 %u\n", (unsigned)rl);
    /* [分块必须 3 字节对齐] 1024 不是 3 的倍数，每块 base64 都带 "=" 填充，
     * 上位机 b64decode 会在第一个 '=' 处截断，只解出第一块（1KB）。
     * 1002 = 3 * 334，除最后一块外都不产生填充。 */
    static uint8_t rbuf[1002];
    static char bbuf[1400];
    size_t sent = 0;
    while (sent < rl) {
        size_t n = (sent + sizeof(rbuf) > rl) ? (rl - sent) : sizeof(rbuf);
        memcpy(rbuf, rle + sent, n);
        size_t olen = 0;
        if (mbedtls_base64_encode((unsigned char*)bbuf, sizeof(bbuf), &olen, rbuf, n) != 0)
            break;
        bbuf[olen] = '\0';
        Serial.println(bbuf);
        Serial.flush();
        vTaskDelay(1);
        sent += n;
    }
    Serial.println("ENDB64");
}

/* 诊断：把相册全屏缓存回传（屏幕上的照片像素）
 * 1/2 横向抽样 = 160x240：竖向保持全分辨率，"黑色横线/黑带"一眼可见；
 * 数据量减半，串口 115200 也能几秒传完。 */
void lcd_app_fullscreen_debug_shot(void)
{
    if (!s_cacheBuf) { Serial.println("[FDBG] no cache buf"); return; }
    const int W = 160, H = 240;                 /* 源 320x240，横向隔点取 */
    static uint8_t* rle = nullptr;
    if (!rle) rle = (uint8_t*)heap_caps_malloc(W * H * 4 + 16, MALLOC_CAP_SPIRAM);
    if (!rle) { Serial.println("[FDBG] alloc fail"); return; }

    size_t rl = 0;
    int i = 0;
    while (i < W * H) {
        int sx = (i % W) * 2, sy = i / W;
        uint16_t c = s_cacheBuf[sy * 320 + sx];
        int run = 1;
        while (i + run < W * H && run < 60000) {
            int nx = ((i + run) % W) * 2, ny = (i + run) / W;
            if (s_cacheBuf[ny * 320 + nx] != c) break;
            ++run;
        }
        rle[rl++] = c & 0xFF; rle[rl++] = c >> 8;
        rle[rl++] = run & 0xFF; rle[rl++] = run >> 8;
        i += run;
    }

    Serial.printf("FDBG %d %d %u\n", W, H, (unsigned)rl);
    /* base64 分块必须是 3 的倍数（1002 = 3x334），否则每块末尾的 '=' 会让
     * 上位机把后续数据当成填充截掉 */
    static uint8_t rbuf[1002];
    static char bbuf[1400];
    size_t sent = 0;
    while (sent < rl) {
        size_t n = (sent + sizeof(rbuf) > rl) ? (rl - sent) : sizeof(rbuf);
        memcpy(rbuf, rle + sent, n);
        size_t olen = 0;
        if (mbedtls_base64_encode((unsigned char*)bbuf, sizeof(bbuf), &olen, rbuf, n) != 0)
            break;
        bbuf[olen] = '\0';
        Serial.println(bbuf);
        Serial.flush();
        vTaskDelay(1);
        sent += n;
    }
    Serial.println("ENDB64");
    Serial.printf("[FDBG] 全屏缓存已回传（idx=%d name=%s）\n",
                  s_cacheIdx, (s_cacheIdx >= 0 && s_cacheIdx < s_count)
                                  ? s_names[s_cacheIdx].c_str() : "-");
}

/* 诊断：把复用 Sprite（游戏/设置/备忘 的整帧缓冲）RLE+Base64 回传 */
void lcd_app_dump_frame(void)
{
    if (!s_sprReady || !s_previewBuf) { Serial.println("IMGERR no-spr"); return; }
    const int N = 320 * 240;
    static uint8_t* rle = nullptr;
    if (!rle) rle = (uint8_t*)heap_caps_malloc(N * 4 + 16, MALLOC_CAP_SPIRAM);
    if (!rle) { Serial.println("IMGERR alloc"); return; }

    size_t rc = 0;
    int i = 0;
    while (i < N && rc + 4 <= (size_t)(N * 4)) {
        uint16_t c = s_previewBuf[i];
        uint32_t run = 1;
        while (i + (int)run < N && s_previewBuf[i + run] == c && run < 0xFFFF) run++;
        rle[rc++] = c & 0xFF; rle[rc++] = c >> 8;
        rle[rc++] = run & 0xFF; rle[rc++] = (run >> 8) & 0xFF;
        i += run;
    }
    Serial.printf("B64 %u\n", (unsigned)rc);
    /* [分块必须按 3 字节对齐] 每个分块单独 base64 编码会带 "=" 填充，
     * 拼接起来就是非法流（上位机 b64decode 会在第一个 = 处截断，只解出 1KB）。
     * 1002 = 3 * 334，除最后一块外都不产生填充 */
    static uint8_t rbuf[1002];
    static char    bbuf[1400];
    size_t sent = 0;
    while (sent < rc) {
        size_t n = (sent + sizeof(rbuf) > rc) ? (rc - sent) : sizeof(rbuf);
        memcpy(rbuf, rle + sent, n);
        size_t olen = 0;
        if (mbedtls_base64_encode((unsigned char*)bbuf, sizeof(bbuf), &olen, rbuf, n) != 0) break;
        bbuf[olen] = '\0';
        Serial.println(bbuf);
        Serial.flush();
        sent += n;
        vTaskDelay(1);
    }
    Serial.println("ENDB64");
}

/* =====================================================================
 * 全屏信息条（叠在照片顶部，1.2s 后自动隐藏）
 * ---------------------------------------------------------------------
 * [花屏残影根因] 旧实现用 fillRect(0,0,320,18) 清底 + 默认字模打印，然后
 *   回贴 18px 高的一条缓存。但字模是"继承"来的：上一屏（九宫格顶栏/设置/
 *   游戏）如果留的是 Font2，字符框 16px 高、起点 y=5，实际画到 y=21，
 *   而恢复区只到 y=17 —— 底下 3~4 行（黑底 + 半个字的笔画）留在屏幕上，
 *   看起来就是一长条花屏。字体状态随上一个 App 不同而不同，所以"有些照片
 *   会恢复、有些不会"。
 * 现在：① 显式复位 TextFont(1)/TextSize(1)，文字框严格落在 18px 内；
 *      ② 隐藏时整帧回贴缓存（320x240）。走的是和翻页完全相同的推屏路径，
 *         屏幕像素与缓存逐点一致，无论字体/叠加内容如何都不会留残迹；
 *      ③ 非阻塞：只记时间戳，由主循环 tick 隐藏，翻页不再卡 1.2s。
 * =================================================================== */
static int      s_infoIdx   = -1;     /* 信息条当前对应的照片序号 */
static uint32_t s_infoUntil = 0;      /* 到期隐藏的时刻 */
static bool     s_infoShown = false;  /* 屏幕上是否有未隐藏的信息条 */

static void gallery_show_info(int listIdx)
{
    /* 无缓存时叠加就没有"原图"可回贴，宁可不显示，避免留下永久污点 */
    if (!s_cacheBuf || s_cacheIdx != listIdx) { s_infoShown = false; return; }

    tft.setTextDatum(TL_DATUM);
    tft.setTextFont(1);                    /* 关键：钉死 8px 字模 */
    tft.setTextSize(1);
    tft.fillRect(0, 0, 320, 18, g_th->osd);
    tft.drawFastHLine(0, 17, 320, g_th->line);
    tft.setCursor(6, 5);
    tft.setTextColor(g_th->osdTx, g_th->osd);
    tft.print(s_names[listIdx] + "  " + (listIdx + 1) + "/" + s_count
              + "   2xCAM=del  MID=back");

    s_infoIdx   = listIdx;
    s_infoUntil = millis() + 1200;
    s_infoShown = true;
}

/* 主循环每次迭代调用：到期就把整帧从缓存回贴，恢复干净画面 */
static void gallery_info_tick(void)
{
    if (!s_infoShown) return;
    if ((int32_t)(millis() - s_infoUntil) < 0) return;
    s_infoShown = false;
    if (!s_cacheBuf || s_cacheIdx != s_infoIdx) return;   /* 照片已换/缓存失效 */
    tft.setSwapBytes(true);
    tft.pushImage(0, 0, 320, 240, s_cacheBuf);            /* 25ms 整帧复原 */
    tft.setSwapBytes(false);
}

/* 残图提示页：SD 上的 JPEG 被截断（掉电/写卡中断）时，TJpgDec 只能解出
 * 前半部分，其余 MCU 块整块是黑的 —— 屏幕上就是"一大片黑色小方格"。
 * 与其把这种图当照片显示，不如明确告诉用户文件坏了。 */
static void gallery_show_broken(int listIdx)
{
    const Theme* th = g_th;
    tft.fillScreen(th->bg);
    tft.setTextDatum(MC_DATUM);
    tft.setTextFont(2);
    tft.setTextSize(1);
    tft.setTextColor(th->danger, th->bg);
    tft.drawString("Image file damaged", 160, 100);
    tft.setTextFont(1);
    tft.setTextColor(th->muted, th->bg);
    tft.drawString(s_names[listIdx] + "  (" + (listIdx + 1) + "/" + s_count + ")",
                   160, 126);
    tft.drawString("2xCAM = delete", 160, 148);
    tft.setTextDatum(TL_DATUM);
    Serial.printf("[GAL] 图片残缺，已给提示页: %s\n", s_names[listIdx].c_str());
}

/* =====================================================================
 * 相册：全屏查看（三级加速）
 *   1. 当前图缓存命中   → 25ms 推屏（秒开）
 *   2. 相邻图预解码命中 → 25ms 推屏 + 缓冲互换（翻页秒开）
 *   3. 都未命中         → TJpgDec scale=8 全图解码（~1s，QSXGA 特性不可避免）
 * 渲染完成后调度相邻照片后台预解码（displayTask 空闲时执行，翻页前已就绪）
 * =================================================================== */
static void galleryFullscreen(int listIdx)
{
    if (listIdx < 0 || listIdx >= s_count) return;

    /* ---- 级别1/2：任一缓冲命中 → 直接推屏（25ms） ---- */
    uint16_t* hitBuf = nullptr;
    if (s_cacheBuf && s_cacheIdx == listIdx) hitBuf = s_cacheBuf;
    else if (s_nextBuf && s_nextIdx == listIdx) hitBuf = s_nextBuf;

    if (hitBuf) {
        /* [重要] TJpgDec 解码输出为小端 RGB565，推屏必须 setSwapBytes(true)，
         * 否则整屏颜色错乱（花屏）。相机 RGB565 帧是大端序（预览用 false），
         * 两条路径字节序不同，不能混用。 */
        tft.setSwapBytes(true);
        tft.pushImage(0, 0, 320, 240, hitBuf);
        tft.setSwapBytes(false);
        /* 命中预解码缓冲：把预解码结果升格为当前缓存，供回翻秒开 */
        if (hitBuf == s_nextBuf && s_cacheBuf) {
            uint16_t* tmp = s_cacheBuf;
            s_cacheBuf = s_nextBuf;
            s_nextBuf  = tmp;
            s_cacheIdx = listIdx;
            s_nextIdx  = -1;
        }
    } else {
        /* ---- 级别3：全图解码 + 写入当前缓存 ----
         * 自适应 scale + 居中：VGA 小图 ~200ms，QSXGA 大图 ~1s+ */
        String path = galleryImagePath(listIdx);
        /* [黑格根因] 残图（缺 EOI）解码不报错，只是后半段 MCU 块全黑，
         * 屏幕上一片黑色小方格。先校验再解码 */
        if (!jpeg_tail_ok(path)) {
            s_cacheIdx = -1;
            gallery_show_broken(listIdx);
            gallery_show_info(listIdx);
            return;
        }
        uint8_t sc; int ox, oy, ow, oh;
        fullJpgGeom(path, sc, ox, oy, ow, oh);
        TJpgDec.setJpgScale(sc);
        TJpgDec.setCallback(tft_output);

        /* 底色先铺满，再居中解码：缓存与屏幕用同一个颜色，回贴才安全
         * （旧版 memset 全黑 + 从 0,0 开始解码 → 下方留一条纯黑带） */
        uint16_t lb = g_th->bg;
        tft.setSwapBytes(true);
        tft.fillScreen(lb);
        frame_fill(s_cacheBuf, lb);

        tft.setCursor(5, 110);
        tft.setTextFont(1);
        tft.setTextSize(2);
        tft.setTextColor(g_th->muted, lb);
        tft.print("Loading...");

        s_caching  = (s_cacheBuf != nullptr);
        s_cacheIdx = listIdx;
        TJpgDec.drawSdJpg(ox, oy, path);
        s_caching = false;
        /* [收尾] 旧版这里漏了复位：解码后 swapBytes 一直是 true，退出相册
         * 后其它 App 的 pushImage 会被误换字节（整屏颜色错乱） */
        tft.setSwapBytes(false);

        /* 解码回调已逐块上屏，画面完整，无需再次推屏 */
    }

    /* [需求] 全屏画面保持干净：不加常驻信息条，进场瞬间叠加 1.2s 提示，
     * 由相册主循环的非阻塞 tick 负责隐藏（不再 vTaskDelay，翻页不被卡住） */
    gallery_show_info(listIdx);

    Serial.printf("[GALLERY] %s（%s）\n", s_names[listIdx].c_str(),
                  hitBuf ? "缓存命中" : "全图解码");

    /* ---- 调度相邻照片预解码：优先下一张，无下一张则预解码上一张 ---- */
    int nb = (listIdx + 1 < s_count) ? listIdx + 1
           : (listIdx - 1 >= 0)      ? listIdx - 1 : -1;
    if (nb >= 0 && s_nextBuf && s_nextIdx != nb) {
        s_prefetchReq    = true;
        s_prefetchBusy   = false;
        s_prefetchTarget = nb;
    }
}

/* 后台预解码：displayTask 空闲时调用（~1s 阻塞相册循环，按键标志不丢） */
static void galleryPrefetchStep(void)
{
    if (!s_prefetchReq || s_prefetchBusy || !s_nextBuf) return;
    if (s_prefetchTarget < 0 || s_prefetchTarget >= s_count) {
        s_prefetchReq = false;
        return;
    }
    s_prefetchBusy = true;
    s_nextIdx = -1;
    String path = galleryImagePath(s_prefetchTarget);
    /* 残图不预解码：解出来也只有一堆黑块，翻页时反而要整屏推一遍 */
    if (!jpeg_tail_ok(path)) {
        s_prefetchReq  = false;
        s_prefetchBusy = false;
        Serial.printf("[GAL] 预解码跳过残图 %s\n", s_names[s_prefetchTarget].c_str());
        return;
    }
    uint8_t sc; int ox, oy, ow, oh;
    fullJpgGeom(path, sc, ox, oy, ow, oh);
    TJpgDec.setJpgScale(sc);
    TJpgDec.setCallback(tft_output);
    /* [残影修复] 预解码缓冲必须先铺满底色：1280x720 只解出 320x180，
     * 旧代码不清缓冲，下面 60 行留着上一张的像素，翻页整屏推出去就是
     * 一条莫名其妙的花带/黑格 */
    frame_fill(s_nextBuf, g_th->bg);
    s_decodeToRam = true;
    s_ramTarget   = s_nextBuf;
    TJpgDec.drawSdJpg(ox, oy, path);
    s_decodeToRam = false;
    s_ramTarget   = nullptr;
    s_nextIdx = s_prefetchTarget;   /* 预解码完成，翻页即可命中 */
    s_prefetchReq  = false;
    s_prefetchBusy = false;
    Serial.printf("[GAL] 预解码 %s 完成（翻页秒开）\n", s_names[s_prefetchTarget].c_str());
}

/* =====================================================================
 * 相机 App 主循环（原 displayTask 预览分支）
 *   [知识点2] 从帧队列取 RGB565 帧（cameraTask 生产，按需启动/停止）
 *   退出流程（g_appExitReq）：删采集任务 + 摄像头 deinit（省电）后返回桌面
 * =================================================================== */
void ui_camera_loop(void)
{
    camera_fb_t* fb = NULL;
    bool blOn      = true;   /* TOP：背光开关（省电） */
    bool cleanView = false;  /* DOWN：纯净预览（隐藏文字） */
    bool rot180    = false;  /* TOP 双击：画面旋转 180° */

    while (true) {
        /* ---- 退出请求：关闭摄像头后回桌面（MID 单击，keyTask 置位） ---- */
        if (g_appExitReq) {
            g_appExitReq = false;
            rtos_stop_camera_task();
            esp_camera_deinit();
            g_cameraReady = false;
            digitalWrite(PIN_LCD_BL, HIGH);   /* 恢复背光（避免黑屏状态下退出） */
            Serial.println("[UI] 退出相机 App：采集任务已删 + 摄像头已断电（省电）");
            return;
        }

        /* ---- 相机 App 按键功能 ----
         * TOP  单击：背光开/关   | TOP  双击：画面旋转 180° | TOP 长按：WiFi 开关
         * DOWN 单击：纯净预览    | DOWN 双击：黑白/彩色预览
         * MID  单击：退出 App    | MID  长按：系统重启
         * CAM  单击：拍照        | CAM  双击/长按：闪光灯拍照 */
        if (g_dispReqRotate) {
            g_dispReqRotate = false;
            rot180 = !rot180;
            tft.setRotation(rot180 ? (LCD_ROTATION + 2) % 4 : LCD_ROTATION);
            Serial.printf("[LCD] 画面旋转 %s\n", rot180 ? "180°" : "复原");
        }
        if (g_btTOPstate == 1) {
            g_btTOPstate = 0;
            blOn = !blOn;
            digitalWrite(PIN_LCD_BL, blOn ? HIGH : LOW);
            Serial.printf("[LCD] 背光 %s\n", blOn ? "开" : "关");
        }
        if (g_btDownstate == 1) {
            g_btDownstate = 0;
            cleanView = !cleanView;
            Serial.printf("[LCD] 纯净预览 %s\n", cleanView ? "开" : "关");
        }

        uint32_t tW0 = micros();
        /* 50ms 超时收帧：给退出请求留出检查窗口（原来 portMAX_DELAY 会卡死退出） */
        if (xQueueReceive(g_frameQueue, &fb, pdMS_TO_TICKS(50)) == pdTRUE && fb) {
            uint32_t t0 = micros();
            s_waitUs += micros() - tW0;
            /* FPS 统计（官方同款算法） */
            frames++;
            unsigned long now = millis();
            if (now - lastMillis >= 2000) {
                g_lcdFps = frames * 1000.0f / (now - lastMillis);
#if SERIAL_PERF
                Serial.printf("[LCD] FPS=%.1f | 推屏=%.1fms 等帧=%.1fms | %dx%d | spr=%d\n",
                              g_lcdFps,
                              frames ? s_pushUs / (float)frames / 1000.0f : 0,
                              frames ? s_waitUs / (float)frames / 1000.0f : 0,
                              fb->width, fb->height, s_sprReady);
#endif
                frames = 0;
                s_pushUs = 0;
                s_waitUs = 0;
                lastMillis = now;
            }

            int w = fb->width;
            int h = fb->height;
            bool jpegFrame = false;
            if (fb->format == PIXFORMAT_JPEG && s_previewBuf) {
                /* [在线切换方案] 预览为 JPEG QVGA 帧（QSXGA 大缓冲在线降档）：
                 * TJpgDec 解码进 Sprite 帧缓冲（复用相册 decode-to-ram 回调），
                 * 换取拍照零 deinit/init（分辨率在线切换） */
                TJpgDec.setJpgScale(1);
                TJpgDec.setCallback(tft_output);
                s_decodeToRam = true;
                s_ramTarget   = s_previewBuf;
                TJpgDec.drawJpg(0, 0, fb->buf, fb->len);
                s_decodeToRam = false;
                s_ramTarget   = nullptr;
                w = 320; h = 240;
                jpegFrame = true;
            }
            if (w <= 320 && h <= 240 && s_previewBuf) {
                /* ---- 整帧写入 Sprite 帧缓冲：RGB565 帧 memcpy / JPEG 已解码直写 ---- */
                if (!jpegFrame)
                    memcpy(s_previewBuf, fb->buf, (size_t)w * h * 2);

                if (s_sprReady && s_spr) {
                    /* ---- OSD 跟随主题：底衬用 osd 色，文字用主题色 ----
                     * [用户反馈] 浅色主题下画面还是"黄字+黑块"，和整个系统
                     * 的浅色界面打架。改成主题色后深浅两套都协调，且底衬
                     * 带描边，亮画面上也不会糊掉 */
                    Drawer og = { s_spr, nullptr };   /* 只写 Sprite，不直绘 */
                    if (!cleanView) {
                        char info[16];
                        sprintf(info, "%dfps", (int)g_lcdFps);
                        ui_osd_chip(og, 4, 4, info, g_th->accent);
                        if (g_wifiState == 1)
                            ui_osd_chip_right(og, 316, 4, "WiFi", g_th->ok);
                        else if (g_wifiState == 0)
                            ui_osd_chip_right(og, 316, 4, "WiFi", g_th->muted);
                    }

                    if (showSavingPopup) {
                        /* 居中弹窗：卡片底 + 强调色描边 + 强调色文字 */
                        og.fillRoundRect(88, 96, 144, 34, 10, g_th->osd);
                        og.drawRoundRect(88, 96, 144, 34, 10, g_th->accent);
                        og.setTextDatum(MC_DATUM);
                        og.setTextFont(2);
                        og.setTextSize(1);
                        og.setTextColor(g_th->accent, g_th->osd);
                        og.drawString("SAVING", 160, 113);
                        og.setTextDatum(TL_DATUM);
                    }

                    if (jpegFrame) s_spr->setSwapBytes(true);    /* TJpgDec 输出小端，推屏需换字节 */
                    s_spr->pushSprite(0, 0);
                    if (jpegFrame) s_spr->setSwapBytes(false);   /* 恢复：相机 RGB565 帧大端 */
                } else if (fb->format == PIXFORMAT_JPEG) {
                    /* ---- 退化直推（无 Sprite）：JPEG 解码直接上屏 ---- */
                    tft.setSwapBytes(true);
                    TJpgDec.setJpgScale(1);
                    TJpgDec.setCallback(tft_output);
                    TJpgDec.drawJpg(0, 0, fb->buf, fb->len);
                    tft.setSwapBytes(false);
                } else {
                    /* ---- 退化直推：官方同款，RGB565 帧直接上屏 ---- */
                    tft.setSwapBytes(false);
                    tft.pushImage(0, 0, 320, 240, (const uint16_t*)fb->buf);
                    char info[16];
                    sprintf(info, "%dfps", (int)g_lcdFps);
                    Drawer og = { nullptr, &tft };
                    ui_osd_chip(og, 4, 4, info, g_th->accent);
                }
                s_pushUs += micros() - t0;
            }

            esp_camera_fb_return(fb);
        }
    }
}

/* =====================================================================
 * 相册 App 主循环（原 displayTask 相册分支：列表菜单 ↔ 全屏查看）
 * 进入时按脏标志重扫 SD；列表态 MID=退出 App，全屏态 MID=回列表
 * =================================================================== */
void ui_gallery_loop(void)
{
    /* ---- 进入相册：重建列表（缓存：仅拍照后（脏标志）才重扫 SD，秒进） ---- */
    uint32_t tB0 = millis();
    if (s_listDirty) {
        buildPhotoList();
        s_listDirty = false;
    }
    Serial.printf("[GAL] 进相册：列表 %d 张（扫描 %lums）\n",
                  s_count, (unsigned long)(millis() - tB0));
    if (s_cur >= s_count) s_cur = s_count - 1;
    if (s_cur < 0) s_cur = 0;
    /* 复位预解码状态：旧索引失效，重新调度 */
    s_nextIdx  = -1;
    s_prefetchReq    = false;
    s_prefetchBusy   = false;
    s_prefetchTarget = -1;
    g_galleryView = 0;
    g_galleryOpenReq = false;
    g_btTOPstate = 0;      /* 丢弃进相册途中误存的按键标志 */
    g_btDownstate = 0;
    g_navBurstUp = g_navBurstDown = 0;   /* 清掉进相册前残留的连发步数 */
    s_gridPendMask = 0;                   /* 新列表：无待解码格 */
    /* [叠加态修复] 缓存命中跳过重绘的判断没考虑"屏幕被桌面/相机覆盖过"：
     * 第二次进相册时页码没变 → 只画选中框，缩略图全不画 → 主界面透出来。
     * 强制失效页缓存，进相册必整页重绘 */
    s_gridSelDrawn = -1;
    s_gridTop     = -1;
    s_infoShown   = false;                /* 进相册时清掉上次残留的信息条状态 */
    galleryGridPage();

    while (true) {
        /* 全屏信息条到点自动隐藏（非阻塞，翻页期间也照常计时） */
        gallery_info_tick();
        /* ---- 退出请求：列表态 MID 单击（keyTask 置位），回桌面 ---- */
        if (g_appExitReq) {
            g_appExitReq = false;
            g_galleryView = 0;
            g_galleryOpenReq = false;
            Serial.println("[GAL] 退出相册 → 回桌面");
            return;
        }
        /* ---- 全屏 → 列表（keyTask MID 单击置位） ---- */
        if (g_galleryListRefreshReq) {
            g_galleryListRefreshReq = false;
            g_galleryView = 0;
            g_galleryOpenReq = false;
            s_infoShown = false;   /* 整页重绘会覆盖信息条，先清状态 */
            s_gridSelDrawn = -1;   /* 全屏覆盖过屏幕：强制整页重绘 */
            s_gridTop     = -1;
            galleryGridPage();
        }

        if (g_galleryView == 0) {
            /* ---------- 九宫格连续滚动列表 ----------
             * 逐格移动；走到可视窗口最右下一格再往下 → 窗口整行上滚，
             * 不是整页跳转（用户要求）。到头/到尾不循环，避免视觉跳变 */
            /* ---- 按键输入：单击 1 格 + 长按连发 ----
             * keyTask 在相册内长按上下键时每 130ms 累加一次步数，这里一次
             * 消费完；必须"逐格"调 galleryGridPage —— 一次跳多格会让
             * delta > GRID_COLS 掉进整页重建（解码 9 张，卡 2s+） */
            int burst = (int)g_navBurstDown - (int)g_navBurstUp;
            g_navBurstDown = 0;
            g_navBurstUp   = 0;
            if (burst == 0) {
                if (g_btDownstate == 1) { g_btDownstate = 0; burst = 1; }
                else if (g_btTOPstate == 1) { g_btTOPstate = 0; burst = -1; }
            }
            if (burst != 0) {
                int dir = (burst > 0) ? 1 : -1;
                int n   = (burst > 0) ? burst : -burst;
                for (int i = 0; i < n && s_count > 0; ++i) {
                    int nv = s_cur + dir;
                    if (nv < 0 || nv >= s_count) break;
                    s_cur = nv;
                    galleryGridPage();          /* 窗口内=刷框；跨行=行滚动 */
                }
                g_galleryOpenReq = false;       /* 连发途中丢弃打开请求，防误触 */
            } else if (s_gridPendMask) {
                /* 本轮没有任何输入：补齐滚动留下的待解码格（连发结束/松手） */
                grid_flush_pending();
            }
            if (g_galleryOpenReq) {            /* CAM 单击：打开选中照片 */
                g_galleryOpenReq = 0;
                g_galleryView = 1;
                galleryFullscreen(s_cur);
            }
        } else {
            /* ---------- 全屏查看态（官方翻页语义） ---------- */
            if (g_galleryDeleteReq) {          /* CAM 双击：删除当前照片 */
                g_galleryDeleteReq = false;
                String p  = "/" + s_names[s_cur];
                String ps = p;  ps.replace(".jpg", "s.jpg");  /* 配套小图 */
                SD.remove(p);
                SD.remove(ps);
                Serial.printf("[GAL] 已删除 %s\n", s_names[s_cur].c_str());
                lcd_app_mark_gallery_dirty();
                buildPhotoList();              /* 重建列表（删除后同步） */
                s_listDirty = false;
                /* 缓存索引全部失效：被删照片与相邻预解码都要作废 */
                s_cacheIdx = -1;
                s_nextIdx  = -1;
                s_prefetchReq    = false;
                s_prefetchTarget = -1;
                if (s_cur >= s_count) s_cur = s_count - 1;
                if (s_cur < 0) s_cur = 0;
                g_galleryView = 0;             /* 回归九宫格列表 */
                s_infoShown = false;
                s_gridSelDrawn = -1;           /* 全屏覆盖过屏幕：强制整页重绘 */
                s_gridTop     = -1;
                galleryGridPage();
                g_galleryOpenReq = false;
                g_btTOPstate = 0;
                g_btDownstate = 0;
            }
            else if (g_btTOPstate == 1) {      /* 上一张（缓存命中秒开） */
                g_btTOPstate = 0;
                if (s_cur > 0) {
                    s_cur--;
                    galleryFullscreen(s_cur);
                }
            }
            if (g_btDownstate == 1) {          /* 下一张 */
                g_btDownstate = 0;
                if (s_cur < s_count - 1) {
                    s_cur++;
                    galleryFullscreen(s_cur);
                }
            }
        }

        /* 全屏空闲：后台预解码相邻照片（翻页秒开的关键） */
        if (g_galleryView == 1 && !g_galleryOpenReq &&
            !g_btTOPstate && !g_btDownstate) {
            galleryPrefetchStep();
        }

        vTaskDelay(pdMS_TO_TICKS(10));   /* 相册模式保持 10ms 响应 */
    }
}
