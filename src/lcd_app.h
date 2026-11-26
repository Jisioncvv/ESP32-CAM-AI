/*
 * =====================================================================
 * lcd_app.h  ——  LCD 屏幕模块接口
 * ---------------------------------------------------------------------
 * 硬件：ST7789 2.4寸 IPS 240x320，SPI 接口
 * 引脚：官方工程同款 SCK=5, MOSI=2, CS=3, DC=4, RST=-1, BL=1
 * 驱动：TFT_eSPI（引脚由 platformio.ini 的 -DTFT_xxx 宏配置）
 * 任务结构：官方 displayTask（预览 ↔ 相册，按键标志消费）
 * =====================================================================
 */
#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>

/* 屏幕驱动对象（ui_app.cpp 桌面/图标绘制共用） */
extern TFT_eSPI tft;

/* 屏幕初始化（SPI + 驱动 + 背光） */
void lcd_app_init(void);

/* 开机自检画面：企鹅 splash + 各模块自检状态列表 */
void lcd_app_show_boot_screen(bool camOk, bool sdOk);

/* 让自检画面停留 ms 毫秒（便于肉眼确认状态列表） */
void lcd_app_hold_boot_screen(int ms);

/* 拍照存卡提示开关（官方 showSavingPopup：预览画面显示 SAVE 提示） */
void lcd_app_set_saving_popup(bool on);

/* 复用相机离屏 Sprite（320x240 全帧缓冲，游戏等 App 用它整帧渲染防闪烁）
 * 返回 nullptr 表示 Sprite 不可用（PSRAM 分配失败），调用方自行回退直绘 */
TFT_eSprite* lcd_app_get_spr(void);

/* 标记相册列表过期（拍照/删除后调用：下次进相册重新扫描 SD） */
void lcd_app_mark_gallery_dirty(void);

/* ---- 诊断（串口命令）：打印九宫格窗口状态 / 回传九宫格缓存画面 ---- */
void lcd_app_gallery_debug_dump(void);
void lcd_app_gallery_debug_shot(void);

/* ---- 诊断：把当前复用 Sprite（游戏/设置/备忘 整帧缓冲）回传成 PNG 数据 ----
 * 协议同桌面截图：B64 <len> → base64(RLE: 颜色lo 颜色hi 步数lo 步数hi) → ENDB64
 * 注意：Sprite 内存是按 ST7789 的大端序存的，上位机解码时需字节交换才是人眼所见 */
void lcd_app_dump_frame(void);

/* ---- 相机 App 主循环（原 displayTask 预览分支，camera App 模式专用） ----
 * 进入后阻塞运行直到 g_appExitReq：释放采集任务 + 摄像头 deinit 后返回 */
void ui_camera_loop(void);

/* ---- 相册 App 主循环（原 displayTask 相册分支：列表菜单 ↔ 全屏查看） ----
 * 进入后阻塞运行直到 g_appExitReq（列表态 MID）返回桌面 */
void ui_gallery_loop(void);
