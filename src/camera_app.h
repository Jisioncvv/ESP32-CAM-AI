/*
 * =====================================================================
 * camera_app.h  ——  摄像头模块接口（官方 EDA-Camera 方案，1:1 对齐）
 * ---------------------------------------------------------------------
 * 预览 = JPEG QVGA fb_count=4
 * 拍照 = JPEG QSXGA(2560x1920) q12 fb_count=1（官方 InitPicConfig，OV5640）
 * 软件配置 = vflip(1) + lenc(1)（官方 InitCameraSoftwareConfig 精简版）
 * 拍照流程 = 官方 displayTask_PhotoSave：删采集任务→切 JPEG→抓帧存卡→切回→重建任务
 * =====================================================================
 */
#pragma once
#include <Arduino.h>

/* 初始化摄像头（RGB565 预览配置）+ 官方软件配置。创建帧队列由 rtos_app 负责 */
bool camera_app_init(void);
bool camera_app_init_jpeg(void);       /* Web 临时拍照：JPEG UXGA 配置 */
bool camera_app_init_jpeg_ex(int framesize);  /* 指定档位：QSXGA 拍照 / VGA 流 */
String camera_app_next_photo_path(void);      /* 下一张照片路径（Web 流帧存卡复用） */
extern bool g_cameraPhotoCfg;          /* 当前摄像头是否处于 JPEG 拍照配置 */
extern volatile int g_photoFs;         /* 设备拍照分辨率（串口 psize 调试切换） */

/* 应用官方软件配置（特效/亮度 + vflip + lenc）到当前传感器 */
void camera_app_apply_soft_config(void);

/* ---------------- 拍照档位（设置 App 可改，NVS 掉电保持） ----------------
 * 分辨率只允许 4:3 与 16:9 两类（实测耗时见 camera_app.cpp）：
 *   HD   1280x720  (16:9) —— 传感器上下裁切，视场比肉眼窄，但拍照最快
 *   XGA  1024x768  (4:3)  —— 全画幅 + 约 3.6s（默认，速度画质平衡）
 *   SXGA 1280x1024 (4:3)  —— 130 万像素，但帧率断崖 → 约 12s，按需选
 */
#define CAM_PSZ_COUNT 3
int  camera_app_photo_size_get(void);            /* 0..CAM_PSZ_COUNT-1 */
void camera_app_photo_size_set(int idx);         /* 切换并写 NVS */
const char* camera_app_photo_size_text(int idx); /* "XGA 1024x768" */
int  camera_app_photo_size_est_sec(int idx);     /* 预估拍照耗时（秒，给设置页显示） */

/* 色彩风格：0=自然（贴近肉眼，默认） 1=鲜艳 */
#define CAM_LOOK_COUNT 2
int  camera_app_photo_look_get(void);
void camera_app_photo_look_set(int idx);
const char* camera_app_photo_look_text(int idx);

/* 白平衡：0=Auto 1=Sunny 2=Cloudy 3=Office 4=Home
 * 暗光/单色场景 Auto 会整屏偏紫，固定预设可让颜色稳定 */
#define CAM_WB_COUNT 5
int  camera_app_photo_wb_get(void);
void camera_app_photo_wb_set(int idx);
const char* camera_app_photo_wb_text(int idx);

/* 定时自拍：0=关 1=3s 2=5s 3=10s（相机 App 内 CAM 长按循环切换，NVS 保持）
 * [为什么需要] 没有定时就只能有人按快门，合影/构图完全拍不了 */
#define CAM_TIMER_COUNT 4
int  camera_app_timer_get(void);                 /* 0..CAM_TIMER_COUNT-1 */
void camera_app_timer_set(int idx);
const char* camera_app_timer_text(int idx);      /* "OFF" / "3s" / "5s" / "10s" */
int  camera_app_timer_seconds(int idx);          /* 0 / 3 / 5 / 10 */

/* 数字亮度偏移：-2..+2（相机 App 内 DOWN 长按循环，NVS 保持，0 为默认）
 * [实测结论 2026-11-26] 同一场景 RGB565 预览 Y≈102，JPEG 成片 Y≈48
 * （QVGA/XGA 都暗，说明是 JPEG(YUV) 通路与 RGB 通路的曲线差异，不是曝光不足）。
 * set_ae_level 调 ±2 对成片几乎无影响；set_brightness 每档约 +0.5EV
 * （Y: 0→48 / +1→64 / +2→80），是真正有效的补偿手段 */
#define CAM_BRI_MIN (-2)
#define CAM_BRI_MAX ( 2)
int  camera_app_brightness_get(void);
void camera_app_brightness_set(int v);           /* 越界截断 + 写 NVS */
int  camera_app_brightness_adjust(int delta);    /* ±2 循环，返回新值 */

/* 拍照完成通知：序号每保存成功一次 +1（相机循环比对后弹成片缩略确认）
 * path 为刚保存的大图路径，小图 +s 后缀 */
int  camera_app_last_shot_seq(void);
const String& camera_app_last_shot_path(void);

/* 启动时从 NVS 载入档位（main 里调用一次） */
void camera_app_load_prefs(void);

/* AWB/AEC 收敛：等 300ms + 丢弃 6 帧过渡帧（重初始化后调用，防预览偏红/偏黄） */
void camera_app_settle_awb(uint8_t frames = 6);

/* 拍照前收敛预算（ms）：实测成片比预览暗 1EV 时用来定位 AE 是否爬到位 */
void     camera_app_settle_ms_set(uint32_t ms);
uint32_t camera_app_settle_ms_get(void);

/* 官方拍照存卡流程（可在按键任务/Web 任务上下文直接调用）：
 *   显示 SAVE 提示 → 删除采集任务 → deinit → JPEG QSXGA 重初始化
 *   → 抓帧 → 存 TF 卡 → 切回 RGB565 → 重建采集任务
 * 返回是否保存成功，photoPath 输出保存路径（可为 nullptr） */
bool camera_app_photo_save(String* photoPath);

/* 等待后台写卡完成（Web /snap 拍完要立即读回文件；超时防卡死） */
void camera_app_wait_write_done(uint32_t timeoutMs);

/* 调试：读回 OV5640 时钟/PLL 寄存器（PLL 调优基线） */
void camera_app_dump_clock_regs(void);

/* 黑白/彩色预览切换（DOWN 双击触发）：state -1=翻转, 0=彩色, 1=黑白 */
void camera_app_set_gray(int state);
