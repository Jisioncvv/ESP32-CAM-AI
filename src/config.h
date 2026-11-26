/*
 * =====================================================================
 * config.h  ——  全局配置与 FreeRTOS 句柄声明
 * ---------------------------------------------------------------------
 * 集中定义：硬件引脚、任务参数、
 *           所有 FreeRTOS 对象的 extern 句柄、全局状态变量、
 *           任务函数声明。
 * 按键语义（桌面 + App 框架 Phase 1）：
 *   桌面：TOP/DOWN=移动图标选择，CAM=打开选中 App，MID 长按=重启
 *   相机 App：CAM=拍照（双击/长按带闪光灯），TOP/DOWN=背光/纯净预览，
 *             MID 单击=退出相机 App（关闭摄像头省电）
 *   相册 App：TOP/DOWN=上下，CAM 单击=打开照片，CAM 双击=删除（全屏态），
 *             MID 单击=返回（全屏→列表→桌面）
 *   占位 App（设置/游戏/备忘）：任意键退回桌面
 * =====================================================================
 */
#pragma once

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include "esp_camera.h"

/* --------------------- 调试开关 --------------------- */
#define SERIAL_PERF   1   /* 1=打印性能剖析日志（[CAM]采集/[LCD]推屏），0=发布静音 */

/* --------------------- 硬件引脚定义 --------------------- */

/* 摄像头引脚（OV5640，立创 ESP32-S3 开发板，与官方工程一致） */
#define PIN_CAM_PWDN    46
#define PIN_CAM_RESET   (-1)   /* 不接，软件不复位 */
/* XCLK=-1：OV5640 模组自带晶振，无需 MCU 供时钟（官方工程同款），
 * 由此 GPIO4 空出，用作屏幕 DC 引脚 */
#define PIN_CAM_XCLK    (-1)
#define PIN_CAM_SIOD    17     /* SCCB SDA */
#define PIN_CAM_SIOC    18     /* SCCB SCL */
#define PIN_CAM_Y2      16     /* D0 */
#define PIN_CAM_Y3      38     /* D1 */
#define PIN_CAM_Y4      15     /* D2 */
#define PIN_CAM_Y5      39     /* D3 */
#define PIN_CAM_Y6      41     /* D4 */
#define PIN_CAM_Y7      40     /* D5 */
#define PIN_CAM_Y8      42     /* D6 */
#define PIN_CAM_Y9      21     /* D7 */
#define PIN_CAM_VSYNC   48
#define PIN_CAM_HREF    47
#define PIN_CAM_PCLK    45

/* TF 卡（独立 HSPI 总线，官方工程同款） */
#define PIN_SD_SCK       13
#define PIN_SD_MISO      14
#define PIN_SD_MOSI      12
#define PIN_SD_CS        11

/* 闪光灯 LED（低电平亮，官方工程同款） */
#define PIN_FLASH_LED    6

/* 4 个按键（上拉，按下接地，官方工程同款引脚）
 * 语义对齐官方 keyTask：状态机识别单击/双击/长按 */
#define PIN_KEY_CAM      10   /* 拍照：单击拍照，双击/长按带闪光灯 */
#define PIN_KEY_TOP      9    /* 上键：特效循环；相册=上一张 */
#define PIN_KEY_MID      8    /* 中键：预览 ↔ 相册 */
#define PIN_KEY_DOWN     7    /* 下键：亮度循环；相册=下一张 */

/* LCD 屏幕（ST7789 2.4寸 IPS 240x320）
 * 注意：TFT_eSPI 实际生效的引脚由 platformio.ini 的 -DTFT_xxx 宏决定，
 * 此处宏仅用于背光等辅助控制，两处必须保持一致 */
#define PIN_LCD_SCK      5
#define PIN_LCD_MOSI     2
#define PIN_LCD_CS       3
#define PIN_LCD_DC       4
#define PIN_LCD_RST      (-1)
#define PIN_LCD_BL       1    /* 背光，高电平点亮 */
#define LCD_ROTATION     1    /* 横屏 320x240 */

/* --------------------- App 框架模式（ui_app.h 定义取值宏） ---------------------
 * g_appMode：0=桌面 1=相机 2=相册 3=设置 4=游戏 5=备忘
 * keyTask 按 g_appMode 分派按键；displayTask（ui_app.cpp）按它调度各 App 主循环 */
extern volatile int  g_appMode;      /* 当前 App 模式（MODE_*） */
extern volatile bool g_appExitReq;   /* 退出当前 App 请求（keyTask 置位，App 主循环消费） */
extern volatile int  g_appSel;       /* 桌面图标选择序号 0..4 */

/* --------------------- 按键/显示状态（官方同款全局标志） --------------------- */
extern volatile int g_btTOPstate;    /* TOP 单击置 1，由相机/相册 App 循环消费清 0 */
extern volatile int g_btDownstate;   /* DOWN 单击置 1，由相机/相册 App 循环消费清 0 */
/* 相册长按连发步数（keyTask 每 130ms 累加，相册主循环一次消费完） */
extern volatile uint8_t g_navBurstUp;
extern volatile uint8_t g_navBurstDown;

/* 相册子状态（相册 App 循环维护，keyTask 的 CAM 键按此分派） */
extern volatile int  g_galleryView;     /* 0=列表菜单, 1=全屏查看 */
extern volatile bool g_galleryOpenReq;  /* CAM 单击请求打开选中照片（列表态） */
extern volatile bool g_galleryDeleteReq; /* CAM 双击请求删除当前照片（全屏态） */
extern volatile bool g_galleryListRefreshReq; /* 全屏→列表 刷新请求（keyTask MID 置位） */
extern volatile bool g_dispReqRotate;   /* TOP 双击请求预览画面旋转 180°（仅相机 App） */
extern volatile int8_t g_wifiState;     /* WiFi 状态：-1=初始 0=关 1=开（屏幕指示用） */

/* --------------------- 任务参数 --------------------- */
#define TASK_CAMERA_PRIO       1
#define TASK_CAMERA_STACK      4096
#define TASK_CAMERA_CORE       0
#define TASK_KEY_PRIO          3    /* 官方：按键任务优先级最高（拍照在其中执行） */
#define TASK_KEY_STACK         8192 /* 拍照流程（esp_camera_init QSXGA）在按键上下文执行 */
#define TASK_KEY_CORE          1
/* 显示任务：官方核1 优先级1（现为 App 调度器，实现在 ui_app.cpp） */
#define TASK_DISPLAY_PRIO      1
#define TASK_DISPLAY_STACK     8192
#define TASK_DISPLAY_CORE      1
/* 后台写卡任务：最低优先级（不与预览/按键抢 CPU），数据在 PSRAM 缓冲中慢慢写 SD */
#define TASK_PHWRITE_PRIO      0
#define TASK_PHWRITE_STACK     8192
#define TASK_PHWRITE_CORE      0

/* --------------------- 队列长度 --------------------- */
#define FRAME_QUEUE_LEN     1   /* 官方：深度 1，只保留最新帧 */

/* --------------------- FreeRTOS 对象 extern 声明 --------------------- */
extern QueueHandle_t       g_frameQueue;        // [知识点2] 帧队列
extern SemaphoreHandle_t   g_cameraMutex;       // [知识点4] 摄像头硬件互斥（官方 camMutex）
extern TaskHandle_t        g_cameraTaskHandle;
extern TaskHandle_t        g_keyTaskHandle;
extern TaskHandle_t        g_displayTaskHandle;

/* --------------------- 全局状态变量 --------------------- */
extern bool        g_cameraReady;       /* 摄像头是否初始化完成 */
extern bool        g_sdReady;           /* SD 卡是否初始化完成 */
extern bool        g_lcdReady;          /* 屏幕是否初始化完成 */
extern float       g_lcdFps;            /* LCD 实时预览帧率（displayTask 统计） */

/* --------------------- 任务函数声明 --------------------- */
extern "C" {
    void cameraTask(void* arg);
    void keyTask(void* arg);
    void displayTask(void* arg);
    void photoWriteTask(void* arg);
}

/* 官方 PhotoSave 重建采集任务（实现位于 rtos_app.cpp） */
void rtos_restart_camera_task(void);
/* 停止并删除采集任务（相机 App 退出/拍照流程前置步骤） */
void rtos_stop_camera_task(void);
