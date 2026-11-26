/*
 * =====================================================================
 * ui_app.h  ——  桌面 + App 框架（Phase 1）
 * ---------------------------------------------------------------------
 * 系统启动后进入"桌面"（图标网格），像一个小型操作系统：
 *   0 相机   —— 进入时才初始化摄像头 + 启动采集任务，退出即关闭省电
 *   1 相册   —— SD 卡照片浏览（列表 + 全屏，原相册逻辑迁移）
 *   2 设置   —— 占位（开发中）
 *   3 游戏   —— 占位（开发中）
 *   4 备忘   —— 占位（开发中）
 * displayTask（config.h extern "C" 声明，实现在 ui_app.cpp）按 g_appMode
 * 调度各 App 主循环；App 退出请求由 keyTask 置 g_appExitReq。
 * =====================================================================
 */
#pragma once
#include <Arduino.h>

/* App 模式取值（g_appMode） */
#define MODE_DESKTOP   0
#define MODE_CAMERA    1
#define MODE_GALLERY   2
#define MODE_SETTINGS  3
#define MODE_GAME      4
#define MODE_NOTES     5
#define MODE_ERROR     6   /* 相机初始化失败等错误占位页 */
#define APP_COUNT      5      /* 桌面图标总数 */

/* 跨 App 按键请求（keyTask 置位，对应 App 主循环消费清 0） */
extern volatile int8_t g_gameDirReq;    /* 游戏：输入请求 1=左转 2=右转 3=暂停 */
extern volatile bool g_noteDeleteReq;  /* 备忘：CAM 单击删除当前选中 */
extern volatile bool g_wifiUiOpenReq;  /* 设置 App：CAM 单击打开 WiFi 管理界面 */

/* UI 框架状态复位（任务创建由 rtos_app 负责） */
void ui_app_init(void);

/* 桌面打开指定 App（keyTask CAM 单击上下文执行；相机 App 含初始化，耗时数百 ms） */
void ui_app_open(int idx);

/* 串口截图（调试）：0='S'裸缓冲测试图案 1='D'完整桌面 2='F'仅白底 3='T'白底+瓷片 */
void ui_app_serial_screenshot(int code);
