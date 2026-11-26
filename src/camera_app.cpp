/*
 * =====================================================================
 * camera_app.cpp  ——  摄像头初始化/软件配置/官方拍照流程
 * ---------------------------------------------------------------------
 * 与官方 EDA-Camera 工程（D:\code\ESP32S3-CAM cameraTask.cpp /
 * displayTask.cpp displayTask_PhotoSave）逐行对齐：
 *   预览配置 InitCameraConfig      = RGB565 + QVGA + fb_count=2 + PSRAM
 *     （实测 19fps 流畅预览；RGB565 无压缩，画质清晰不泛白）
 *   拍照配置 InitPicConfig         = JPEG + QSXGA + q12 + fb_count=2（OV5640）
 *   软件配置 InitCameraSoftwareConfig = 特效/亮度 + vflip(1) + lenc(1)
 *   拍照流程 photo_save = 停采集任务 → deinit → JPEG 重初始化 → 抓 QSXGA
 *     → 在线切 VGA 补小图 → 拷 PSRAM 缓冲 → 切回预览配置 → 重建采集任务
 *     （[实测结论] esp32-camera 不支持 JPEG↔RGB565 运行时切换；JPEG 内切
 *      分辨率后传感器时钟不恢复（QVGA 预览仅 1.2fps），在线方案已放弃）
 * =====================================================================
 */
#include "config.h"
#include "camera_app.h"
#include "sd_app.h"
#include "lcd_app.h"
#include "ui_app.h"
#include <SD.h>
#include <WiFi.h>    /* 拍照档位自适应：WiFi 在线时 QSXGA DMA 会被挤死 */
#include "web_app.h" /* web_app_wifi_busy：WiFi 连接中拍照降 UXGA（竞态） */

bool g_cameraReady = false;

/* 官方同名全局：摄像头配置结构体与传感器指针 */
static camera_config_t config;
static sensor_t* sensor = nullptr;

/* =====================================================================
 * 官方 cameraTask_InitCameraConfig —— 回退用预览配置（RGB565 QVGA）
 * 仅在传感器在线切换失败时使用（官方 deinit 流程的恢复配置）
 * =================================================================== */
static void InitCameraConfig(void)
{
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer   = LEDC_TIMER_0;
    config.pin_d0       = PIN_CAM_Y2;
    config.pin_d1       = PIN_CAM_Y3;
    config.pin_d2       = PIN_CAM_Y4;
    config.pin_d3       = PIN_CAM_Y5;
    config.pin_d4       = PIN_CAM_Y6;
    config.pin_d5       = PIN_CAM_Y7;
    config.pin_d6       = PIN_CAM_Y8;
    config.pin_d7       = PIN_CAM_Y9;
    config.pin_xclk     = PIN_CAM_XCLK;
    config.pin_pclk     = PIN_CAM_PCLK;
    config.pin_vsync    = PIN_CAM_VSYNC;
    config.pin_href     = PIN_CAM_HREF;
    config.pin_sccb_sda = PIN_CAM_SIOD;
    config.pin_sccb_scl = PIN_CAM_SIOC;
    config.pin_pwdn     = PIN_CAM_PWDN;
    config.pin_reset    = PIN_CAM_RESET;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_RGB565;  /* RGB565 无损预览（官方 InitCameraConfig）：无 JPEG 压缩，画面清晰不泛白 */
    config.frame_size   = FRAMESIZE_QVGA;
    config.jpeg_quality = 12;                /* 仅拍照 JPEG 时生效，RGB565 预览忽略此值 */
    config.fb_count     = 2;                 /* 官方 2 缓冲：RGB565 QVGA 帧约 150KB，2 缓冲足够流畅 */
    config.fb_location  = CAMERA_FB_IN_PSRAM;
}

/* =====================================================================
 * 初始化主配置（原官方 cameraTask_InitPicConfig）—— JPEG QSXGA（OV5640）
 * 帧缓冲按 QSXGA 分配后常驻：预览(QVGA)和拍照(QSXGA)都在同一组缓冲上跑，
 * 拍照只需 set_pixformat/set_framesize 切传感器寄存器（几十 ms），
 * 不再 deinit/init（旧流程每次要 ~3s，且是 gdma 崩溃的源头）
 * =================================================================== */
static void InitPicConfig(framesize_t fs = FRAMESIZE_QSXGA)
{
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer   = LEDC_TIMER_0;
    config.pin_d0       = PIN_CAM_Y2;
    config.pin_d1       = PIN_CAM_Y3;
    config.pin_d2       = PIN_CAM_Y4;
    config.pin_d3       = PIN_CAM_Y5;
    config.pin_d4       = PIN_CAM_Y6;
    config.pin_d5       = PIN_CAM_Y7;
    config.pin_d6       = PIN_CAM_Y8;
    config.pin_d7       = PIN_CAM_Y9;
    config.pin_xclk     = PIN_CAM_XCLK;
    config.pin_pclk     = PIN_CAM_PCLK;
    config.pin_vsync    = PIN_CAM_VSYNC;
    config.pin_href     = PIN_CAM_HREF;
    config.pin_sccb_sda = PIN_CAM_SIOD;
    config.pin_sccb_scl = PIN_CAM_SIOC;
    config.pin_pwdn     = PIN_CAM_PWDN;
    config.pin_reset    = PIN_CAM_RESET;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;
    /* OV5640 → QSXGA 2560x1920（官方分支；2640 为 SXGA，默认 UXGA） */
    config.frame_size   = fs;
    config.jpeg_quality = 12;
    config.fb_count     = 2;                 /* 2 缓冲：预览流畅 + 停任务瞬间容错 1 帧滞留 */
    config.fb_location  = CAMERA_FB_IN_PSRAM;
}

/* =====================================================================
 * 官方 cameraTask_InitCameraSoftwareConfig —— 传感器软件配置（画质增强版）
 * 基础：vflip=1（OV5640 方向校准）+ lenc=1（镜头阴影校正）
 * 画质链（对齐手机 ISP 流水线思路，全部走 OV5640 自带硬件）：
 *   bpc/wpc    = 黑点/白点缺陷像素校正（去噪点坏点）
 *   raw_gma    = 传感器 gamma 校正（暗部细节，接近人眼响应）
 *   aec2       = AEC2 动态范围增强（强光/逆光过曝抑制，暗光增益配合）
 *   awb_gain   = 自动白平衡增益（防偏色）
 *   contrast/saturation/sharpness = 提亮对比/饱和/锐化（手机直出味的来源）
 *   denoise    = 轻度降噪（暗环境噪声抑制）
 * =================================================================== */
void camera_app_apply_soft_config(void)
{
    sensor = esp_camera_sensor_get();
    if (sensor == nullptr) return;

    /* 方向校准实测：C 组 = vflip 1 为正确方向（与官方 OV5640 分支一致） */
    sensor->set_vflip(sensor, 1);
    sensor->set_lenc(sensor, 1);

    /* ---- 缺陷校正 / gamma / 动态范围（画质底子） ---- */
    sensor->set_bpc(sensor, 1);        /* 黑点校正 */
    sensor->set_wpc(sensor, 1);        /* 白点校正 */
    sensor->set_raw_gma(sensor, 1);    /* 传感器 gamma 曲线 */
    sensor->set_aec2(sensor, 1);       /* AEC2：宽动态，强光不过曝 */
    sensor->set_awb_gain(sensor, 1);   /* AWB 增益 */
    sensor->set_wb_mode(sensor, 0);    /* 自动白平衡 */

    /* ---- 观感增强（手机直出风格） ----
     * [偏红修复] 实测饱和度 +1 会放大 AWB 收敛残差（画面明显偏红），回落到 0；
     * AWB 落定前的过渡帧由 camera_app_settle_awb() 丢弃，不再依赖饱和度掩盖 */
    sensor->set_contrast(sensor, 1);     /* -2..2：对比度 +1 */
    sensor->set_saturation(sensor, 0);   /* -2..2：饱和度默认（+1 会放大偏红） */
    sensor->set_sharpness(sensor, 2);    /* -2..2：锐化 +2 */
    sensor->set_denoise(sensor, 2);      /* 降噪加强（暗光高增益噪点明显） */
    /* 限制自动增益上限：默认会一路拉满产生密集彩色噪点；
     * 16X 上限换取更干净的暗部（代价：极暗场景画面会略暗） */
    sensor->set_gainceiling(sensor, GAINCEILING_16X);
    sensor->set_ae_level(sensor, 0);     /* AE 目标默认（强光下由 aec2 压过曝） */
}

/* =====================================================================
 * [偏色修复] 传感器重初始化后 AWB/AEC 全部从头收敛，前几秒画面常呈
 * 偏红/偏黄（用户实测：拍照后/晃动后预览随机出现偏红或偏黄两种状态）。
 * 等 300ms 让 AWB 统计稳定，再丢弃 6 帧过渡帧 —— 预览恢复的首帧
 * 就是白平衡落定后的画面，不会再闪出偏色场景
 * （必须在启动 cameraTask 前调用，fb_get 直接取帧不经过队列）
 * =================================================================== */
void camera_app_settle_awb(uint8_t frames)
{
    vTaskDelay(pdMS_TO_TICKS(300));
    for (uint8_t i = 0; i < frames; ++i) {
        camera_fb_t* fb = esp_camera_fb_get();
        if (fb) esp_camera_fb_return(fb);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* =====================================================================
 * 调试：读回 OV5640 时钟/PLL/帧率寄存器，建立调优基线
 * 帧率公式：fps = PCLK / (HTS × VTS)
 *   PLL 控制：0x3034(M倍频) 0x3035(P预分频/S系统分频) 0x3036 0x3037(PCLK分频)
 *   总尺寸   ：0x380C/0x380D=HTS  0x380E/0x380F=VTS
 * =================================================================== */
void camera_app_dump_clock_regs(void)
{
    sensor = esp_camera_sensor_get();
    if (sensor == nullptr) return;
    #define RD(reg) (sensor->get_reg(sensor, (reg), 0xFF))
    Serial.printf("[PLL] 0x3034=%02X 0x3035=%02X 0x3036=%02X 0x3037=%02X 0x3038=%02X 0x3039=%02X\n",
                  RD(0x3034), RD(0x3035), RD(0x3036), RD(0x3037), RD(0x3038), RD(0x3039));
    Serial.printf("[PLL] 0x3108=%02X HTS=0x%02X%02X VTS=0x%02X%02X\n",
                  RD(0x3108), RD(0x380D), RD(0x380C), RD(0x380F), RD(0x380E));
    #undef RD
}

/* 黑白/彩色预览切换（DOWN 双击）：-1=翻转当前状态 */
void camera_app_set_gray(int state)
{
    static bool s_gray = false;
    if (state == -1) s_gray = !s_gray;
    else             s_gray = (state != 0);
    sensor_t* s = esp_camera_sensor_get();
    if (s) {
        s->set_special_effect(s, s_gray ? 1 : 0);   /* 0=无特效 1=灰度 */
        Serial.printf("[CAM] 预览模式：%s\n", s_gray ? "黑白" : "彩色");
    }
}

/* =====================================================================
 * PLL 调优（RGB565 QVGA 实测记录）：
 * 驱动默认 0x3035=0x11 0x3036=0x08 时 PCLK≈9.7MHz，帧周期 210ms（4.7fps）。
 * 单改 sys_div 无效果（帧率不变），帧率与 0x3036 倍频成正比。
 * 现改：倍频 8→16（0x3036: 0x08→0x10），PCLK≈19.4MHz（S3 8-bit DVP 稳定上限内），
 * 目标帧周期 105ms ≈ 9.5fps，画质无损（仍为 RGB565 直出）。
 * =================================================================== */
void camera_app_tune_pll(void)
{
    sensor = esp_camera_sensor_get();
    if (sensor == nullptr) return;
    Serial.println("[PLL] 倍频 8→16（0x3036: 0x08→0x10），sys_div 保持 2");
    sensor->set_reg(sensor, 0x3036, 0xFF, 0x10);   /* PLL 倍频 ×2：PCLK 9.7→19.4MHz */
    camera_app_dump_clock_regs();
}

/* =====================================================================
 * 初始化：官方 cameraTask_Init 简化版（RGB565 QVGA 预览配置，实测 19fps）
 * 队列由 rtos_app 创建
 * =================================================================== */
bool camera_app_init(void)
{
    /* [双拍重启根因修复] 驱动可能已经在线（拍照流程恢复预览后 g_cameraReady
     * 未同步为 true 的历史状态）：此时再 esp_camera_init 会 probe 失败
     * (0x103 ESP_ERR_INVALID_STATE) 并打挂 gdma，采集任务随即 fb_get 空指针
     * 崩溃重启。用 esp_camera_sensor_get() 探测驱动是否活着，活着且已是
     * RGB565 预览配置 → 只补标志，绝不二次 init */
    sensor_t* alive = esp_camera_sensor_get();
    if (alive && !g_cameraReady) {
        if (alive->pixformat == PIXFORMAT_RGB565 &&
            alive->status.framesize == FRAMESIZE_QVGA) {
            Serial.println("[CAM] 驱动已在线（标志未同步），补标志复用预览配置");
            g_cameraReady = true;
            g_cameraPhotoCfg = false;
            return true;
        }
        Serial.println("[CAM] 驱动在其它配置，先 deinit 再重建");
        esp_camera_deinit();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    /* [Web 预览兼容] 摄像头可能还停留在 Web 的 JPEG 拍照配置（/preview 启动
     * 且未断电）：预览必须 RGB565 配置，先彻底重建 */
    if (g_cameraReady && g_cameraPhotoCfg) {
        Serial.println("[CAM] 检测到 Web 拍照配置残留，重建预览配置");
        esp_camera_deinit();
        g_cameraReady = false;
        g_cameraPhotoCfg = false;
    }
    if (g_cameraReady) return true;      /* 已在预览配置（App 内二次进入） */
    Serial.println("[CAM] 初始化中（RGB565 QVGA 预览配置）...");
    InitCameraConfig();
    if (esp_camera_init(&config) != ESP_OK) {
        Serial.println("[CAM] init failed!");
        g_cameraReady = false;
        return false;
    }
    Serial.println("[CAM] Camera init ok!");
    camera_app_apply_soft_config();
    camera_app_tune_pll();           /* PLL 倍频 ×2：PCLK 9.7→19.4MHz → 19fps */
    camera_app_settle_awb();         /* 丢弃 AWB 收敛过渡帧（开机首屏不偏色） */
    g_cameraReady = true;
    return true;
}

/* [Web 拍照修复] Web 临时启动直接用 JPEG QSXGA 拍照配置：
 * 旧流程 = 临时 init 预览配置 → photo_save 立刻 deinit 再 init 拍照配置，
 * 1 秒内 init→deinit→init 猛切实测会打挂摄像头驱动（gdma 断链），
 * fb_get 卡死 ~25s / 28s 才拍出一张。直接拍照配置则无切换 */
bool g_cameraPhotoCfg = false;       /* 当前摄像头是否已处于 JPEG 拍照配置 */

bool camera_app_init_jpeg_ex(int framesize)
{
    Serial.printf("[CAM] 初始化中（JPEG %s 拍照配置，Web）...\n",
                  framesize == FRAMESIZE_QSXGA ? "QSXGA" :
                  framesize == FRAMESIZE_VGA   ? "VGA 流" : "UXGA");
    InitPicConfig((framesize_t)framesize);
    if (esp_camera_init(&config) != ESP_OK) {
        Serial.println("[CAM] JPEG init failed!");
        g_cameraPhotoCfg = false;
        return false;
    }
    camera_app_apply_soft_config();
    camera_app_tune_pll();           /* PLL 倍频 ×2：不补它 QSXGA 单帧可能拖到 20s+ */
    camera_app_settle_awb(1);        /* Web 场景只丢 1 帧：首帧延迟从 ~14s 压到 ~3s */
    g_cameraReady = true;
    g_cameraPhotoCfg = true;
    return true;
}

bool camera_app_init_jpeg(void)
{
    return camera_app_init_jpeg_ex(FRAMESIZE_UXGA);
}

/* =====================================================================
 * 后台写卡（拍照停顿优化的核心）：
 *   前台 photo_save 只做：抓帧 → 拷贝进 PSRAM 缓冲 → 恢复预览，全程无 SD IO；
 *   photoWriteTask（常驻，核0 优先级0）轮询请求标志慢慢写 SD。
 *   预期把拍照停顿从 5~7.6s 压到 ~2-3s（SD 写 1MB/s 是旧流程大头）。
 * 竞态防护：photo_save 提交前等 s_bgBusy/s_bgReq 双标志清零（上一次写完），
 *   才允许覆盖缓冲；meta（path/len）读写用互斥锁。
 * =================================================================== */
#define PHOTO_BG_BIG_SIZE    (1024 * 1024)  /* QSXGA q12 实测最大 ~827KB（复杂场景），留余量到 1MB */
#define PHOTO_BG_SMALL_SIZE  (128 * 1024)   /* VGA q10 小图 ~60KB */

static uint8_t* s_bgBigBuf   = nullptr;
static uint8_t* s_bgSmallBuf = nullptr;
static size_t   s_bgBigLen   = 0;
static size_t   s_bgSmallLen = 0;
static String   s_bgPath;
static volatile bool s_bgReq  = false;   /* 缓冲中有待写数据 */
static volatile bool s_bgBusy = false;   /* 正在写卡 */
static SemaphoreHandle_t s_bgMetaMutex = nullptr;  /* 保护 path/len 元数据 */
static bool s_bgOk = false;              /* 后台写卡资源是否就绪 */

void photoWriteTask(void* arg)
{
    (void)arg;
    s_bgMetaMutex = xSemaphoreCreateMutex();
    s_bgBigBuf   = (uint8_t*)heap_caps_malloc(PHOTO_BG_BIG_SIZE, MALLOC_CAP_SPIRAM);
    s_bgSmallBuf = (uint8_t*)heap_caps_malloc(PHOTO_BG_SMALL_SIZE, MALLOC_CAP_SPIRAM);
    s_bgOk = (s_bgMetaMutex && s_bgBigBuf && s_bgSmallBuf);
    Serial.printf("[PHOTO] 后台写卡任务 %s（大图缓冲 %s 小图缓冲 %s）\n",
                  s_bgOk ? "就绪" : "失败（回退同步写）",
                  s_bgBigBuf ? "OK" : "FAIL", s_bgSmallBuf ? "OK" : "FAIL");

    for (;;) {
        if (s_bgReq && s_bgOk) {
            s_bgBusy = true;
            /* 先写小图（相册秒开优先）再写大图 */
            String smallPath = s_bgPath;
            smallPath.replace(".jpg", "s.jpg");
            if (s_bgSmallLen) sd_app_save_photo(smallPath, s_bgSmallBuf, s_bgSmallLen);
            if (s_bgBigLen)   sd_app_save_photo(s_bgPath, s_bgBigBuf, s_bgBigLen);
            s_bgBigLen = s_bgSmallLen = 0;
            s_bgReq = false;
            lcd_app_mark_gallery_dirty();   /* 新照片入册：下次进相册重扫 */
            s_bgBusy = false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* 等待后台写卡完成（Web /snap 需要立即读回刚拍的照片文件） */
void camera_app_wait_write_done(uint32_t timeoutMs)
{
    uint32_t t0 = millis();
    while ((s_bgBusy || s_bgReq) && millis() - t0 < timeoutMs) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* 照片编号缓存：开机首次拍照扫描 SD 取最大编号，之后递增。
 * 后台写卡期间再拍也不会算错编号（不再实时扫卡，扫卡会漏掉未写盘的照片） */
static int s_photoSeq = -1;
static String next_photo_path(void);         /* 前置声明：Web 流帧存卡复用 */
String camera_app_next_photo_path(void) { return next_photo_path(); }
static String next_photo_path(void)
{
    if (s_photoSeq < 0) {
        int maxIdx = 0;
        File root = SD.open("/");
        if (root) {
            while (true) {
                File f = root.openNextFile();
                if (!f) break;
                if (!f.isDirectory()) {
                    String name = String(f.name());
                    if (name.startsWith("photo_") && name.endsWith(".jpg")) {
                        int s = name.indexOf('_'), e = name.indexOf('.');
                        if (s > 0 && e > s) {
                            int n = name.substring(s + 1, e).toInt();
                            if (n > maxIdx) maxIdx = n;
                        }
                    }
                }
                f.close();
            }
            root.close();
        }
        s_photoSeq = maxIdx;
    }
    ++s_photoSeq;
    return "/photo_" + String(s_photoSeq) + ".jpg";
}

/* =====================================================================
 * 官方 displayTask_PhotoSave —— 拍照流程（后台写卡版）
 * 前台：抓大图拷缓冲 → 抓小图拷缓冲 → 恢复预览（无 SD IO，前台 ~2.3s）
 * 后台：photoWriteTask 写大图/小图入卡
 * [实测结论] esp32-camera 在线切换两条路都不通：
 *   1) JPEG↔RGB565 运行时切换 —— 驱动不支持，切后收不到帧
 *   2) JPEG 内切分辨率 —— 帧率掉到 1.2fps（传感器时钟不随分辨率恢复）
 * 故拍照维持官方 deinit 流程，提速靠「写卡后台化」（前台无 SD IO）
 * =================================================================== */
/* [代码历史比对结论 + 实机验证] 备份版（camera_app.cpp 2026-10-21，拍照 2~3s）
 * 的拍照路径：QSXGA + fb_count=1 + 不调 PLL + 无预热帧 + 同步写卡。
 * 照搬实测失败：fb_count=1 在当前 esp32-camera 构建下 fb_get 连续 null
 * （QSXGA 15.7s / SXGA 15.3s 全部拍空），故必须保留 fb_count=2。
 * 保留的两处提速：① 去掉拍照路径的 tune_pll ② 预热帧与主帧之间不等待。
 * 默认 SXGA 1280x1024：实测 6.0s（QSXGA 10~14s、UXGA 12s），130 万像素。 */
volatile int g_photoFs = FRAMESIZE_HD;
bool camera_app_photo_save(String* photoPath)
{
    uint32_t tAll = millis();                /* 总耗时统计（优化照片流程用） */
    web_app_stream_hold(true);               /* 暂停网页取景流（抓帧让路） */
    lcd_app_set_saving_popup(true);          /* 官方 showSavingPopup = true */
    vTaskDelay(pdMS_TO_TICKS(100));          /* 官方：给屏幕时间显示保存提示 */

    /* [致命延迟修复] 原写法最长等 20 秒：只要 WiFi 正在重连，按拍照键就
     * 卡住十几二十秒——这就是"有时突然特别慢"的直接原因。压到 3 秒上限 */
    uint32_t tW = millis();
    while (web_app_wifi_busy() && millis() - tW < 3000)
        vTaskDelay(pdMS_TO_TICKS(100));

    /* 1. 非 QSXGA 拍照态（预览 RGB565 / Web UXGA 取景残留）→ 重建 QSXGA
     * 距上次 init 通常 >1s（预览/取景跑过一段时间），非 1s 内猛切；
     * Web 冷启动拍照由 web_ensure_camera 直接 QSXGA，不进本分支 */
    sensor_t* sChk = esp_camera_sensor_get();
    int curFs = (g_cameraReady && sChk) ? (int)sChk->status.framesize : -1;
    framesize_t targetFs = (framesize_t)g_photoFs;
    if (!g_cameraReady || !g_cameraPhotoCfg || curFs != (int)targetFs) {
        if (g_cameraReady) rtos_stop_camera_task();
        uint32_t t1 = millis();
        Serial.printf("[T] 停预览 %ums\n", millis() - tAll);
        esp_camera_deinit();
        g_cameraReady = false;
        uint32_t t2 = millis();
        Serial.printf("[T] deinit %ums\n", t2 - tAll);
        InitPicConfig(targetFs);
        esp_err_t err = esp_camera_init(&config);
        uint32_t t3 = millis();
        Serial.printf("[T] init fs=%d %ums\n", (int)targetFs, t3 - tAll);
        camera_app_apply_soft_config();
        if (err != ESP_OK) {
            Serial.printf("Camera reinit JPEG failed: %d\n", err);
            lcd_app_set_saving_popup(false);
            /* 尽力恢复预览，避免摄像头留在坏状态 */
            esp_camera_deinit();
            vTaskDelay(pdMS_TO_TICKS(100));
            InitCameraConfig();
            g_cameraReady = (esp_camera_init(&config) == ESP_OK);
            if (g_cameraReady) camera_app_apply_soft_config();
            g_cameraPhotoCfg = false;
            if (g_appMode == MODE_CAMERA && g_cameraReady) rtos_restart_camera_task();
            web_app_stream_hold(false);
            return false;
        }
        /* PLL 倍频按档位启用（实测数据）：
         *   ≥SXGA 不倍频 → fb_get 全 null 拍空；倍频后 QSXGA 11.8s / FHD 12.0s
         *   ≤HD 倍频反而拖慢：XGA 2.6s(不倍频) vs 6.5s(倍频) */
        if (targetFs >= FRAMESIZE_SXGA) camera_app_tune_pll();
        /* 预热帧：吸收 init 后的首帧延迟 */
        camera_fb_t* warm = esp_camera_fb_get();
        if (warm) esp_camera_fb_return(warm);
        g_cameraReady = true;            /* 拍照配置已就绪（驱动在线） */
        g_cameraPhotoCfg = true;
    }

    /* 2. 抓大图（QSXGA 全画幅）→ 数据拷入 PSRAM 写卡缓冲，SD 交给后台。
     * 重试 3 次：重初始化后首帧偶发 fb_get null（~5s 白等），重试可自愈 */
    bool ok = false;
    String path = next_photo_path();
    for (int i = 0; i < 3 && !ok; ++i) {
        uint32_t tF = millis();
        camera_fb_t* fb = esp_camera_fb_get();
        Serial.printf("[PHOTO] 大图 fb_get %s @%ums（耗时 %ums）\n",
                      fb ? "OK" : "null", millis() - tAll, millis() - tF);
        if (!fb) {
            Serial.printf("[PHOTO] fb_get null（第 %d 次），300ms 后重试\n", i + 1);
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        }
        Serial.printf("[PHOTO] 大图 %uB @%ums\n", (unsigned)fb->len, millis() - tAll);
        if (s_bgOk && fb->len <= PHOTO_BG_BIG_SIZE) {
            xSemaphoreTake(s_bgMetaMutex, portMAX_DELAY);
            memcpy(s_bgBigBuf, fb->buf, fb->len);
            s_bgBigLen = fb->len;
            xSemaphoreGive(s_bgMetaMutex);
            ok = true;
        } else if (s_bgOk) {
            Serial.println("[PHOTO] 大图超出后台缓冲，回退同步写");
            ok = sd_app_save_photo(path, fb->buf, fb->len);
        } else {
            ok = sd_app_save_photo(path, fb->buf, fb->len);   /* 后台未就绪：同步写 */
        }
        esp_camera_fb_return(fb);
    }

    /* 3.5 传感器在线切换 VGA JPEG 补拍小图（相册秒开用），数据同样只拷缓冲
     * （JPEG 格式内切 framesize 可用：旧版实测能取到 VGA 帧） */
    if (ok) {
        uint32_t tS = millis();
        sensor_t* s = esp_camera_sensor_get();
        if (s) {
            s->set_pixformat(s, PIXFORMAT_JPEG);
            s->set_framesize(s, FRAMESIZE_VGA);
            s->set_quality(s, 10);            /* q10：小图质量略高于大图 q12 */
            vTaskDelay(pdMS_TO_TICKS(150));   /* 等传感器应用新格式（实测150ms足够） */
            /* 丢掉格式切换过渡期的旧尺寸帧，取到 VGA 帧为止 */
            for (int i = 0; i < 5; ++i) {
                uint32_t tA = millis();
                camera_fb_t* sfb = esp_camera_fb_get();
                Serial.printf("[T] 小图过渡帧%d %ums（%dx%d）\n", i,
                              millis() - tA, sfb ? sfb->width : 0, sfb ? sfb->height : 0);
                if (!sfb) break;
                bool good = (sfb->format == PIXFORMAT_JPEG && sfb->width == 640);
                if (good) {
                    camera_fb_t* smallFb = esp_camera_fb_get();
                    if (smallFb) {
                        if (s_bgOk && smallFb->len <= PHOTO_BG_SMALL_SIZE) {
                            xSemaphoreTake(s_bgMetaMutex, portMAX_DELAY);
                            memcpy(s_bgSmallBuf, smallFb->buf, smallFb->len);
                            s_bgSmallLen = smallFb->len;
                            xSemaphoreGive(s_bgMetaMutex);
                        } else if (!s_bgOk) {
                            String smallPath = path;
                            smallPath.replace(".jpg", "s.jpg");
                            sd_app_save_photo(smallPath, smallFb->buf, smallFb->len);
                        }
                        esp_camera_fb_return(smallFb);
                        Serial.println("[PHOTO] 小图已抓取（相册秒开用）");
                    }
                }
                esp_camera_fb_return(sfb);
                if (good) break;
            }
        }
        Serial.printf("[T] 小图阶段合计 %ums\n", millis() - tS);
    }

    /* 4. 官方：切回预览配置（此时 SD 完全空闲，恢复更干净） */
    uint32_t tR = millis();
    esp_camera_deinit();
    vTaskDelay(pdMS_TO_TICKS(100));
    InitCameraConfig();
    esp_err_t rerr = esp_camera_init(&config);
    if (rerr != ESP_OK) {
        Serial.printf("[PHOTO] 预览恢复失败(%d)，200ms 后重试\n", rerr);
        esp_camera_deinit();
        vTaskDelay(pdMS_TO_TICKS(200));
        InitCameraConfig();
        rerr = esp_camera_init(&config);
    }
    /* [双拍重启根因] 这里必须把 g_cameraReady 同步为 true：旧代码恢复预览后
     * 标志仍是 false，第二次按拍照会走"临时初始化"分支二次 esp_camera_init，
     * probe 失败打挂 gdma → 采集任务 fb_get 空指针 → 卡死重启 */
    g_cameraReady = (rerr == ESP_OK);
    if (g_cameraReady) {
        camera_app_apply_soft_config();
        camera_app_tune_pll();       /* [修复] 开机有、拍照恢复漏了：缺它预览掉回 9.5fps */
        camera_app_settle_awb(2);    /* [提速] 恢复预览只丢 2 帧（原 6 帧多花 ~0.5s；AWB 会自收敛） */
    } else {
        Serial.println("[PHOTO] 预览配置恢复失败：保持采集任务停止（不重启，避免崩驱动）");
    }
    g_cameraPhotoCfg = false;
    Serial.printf("[T] 恢复预览 %ums\n", millis() - tR);

    /* 2. 提交后台写卡：先等上一次写完（防覆盖缓冲），再置请求标志 */
    if (ok && s_bgOk) {
        uint32_t tW = millis();
        while ((s_bgBusy || s_bgReq) && millis() - tW < 3000) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        xSemaphoreTake(s_bgMetaMutex, portMAX_DELAY);
        s_bgPath = path;
        s_bgReq = true;
        xSemaphoreGive(s_bgMetaMutex);
    }

    /* 3. 官方：重建 cameraTask（仅相机 App 激活且预览配置恢复成功时） */
    lcd_app_set_saving_popup(false);
    if (g_appMode == MODE_CAMERA && g_cameraReady) rtos_restart_camera_task();

    if (ok) {
        Serial.println("[PHOTO] 保存成功（SD 写入后台进行中）");
        lcd_app_mark_gallery_dirty();
    }
    web_app_stream_hold(false);          /* 恢复网页取景流 */
    /* 耗时统计：前台不再包含 SD 写入（后台执行） */
    Serial.printf("[PHOTO] 前台耗时 %lums（抓帧+拷缓冲+恢复预览；写卡后台执行）\n",
                  (unsigned long)(millis() - tAll));
    if (ok && photoPath) *photoPath = path;
    return ok;
}
