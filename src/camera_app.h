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

/* AWB/AEC 收敛：等 300ms + 丢弃 6 帧过渡帧（重初始化后调用，防预览偏红/偏黄） */
void camera_app_settle_awb(uint8_t frames = 6);

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
