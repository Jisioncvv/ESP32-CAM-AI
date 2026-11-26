/*
 * =====================================================================
 * main.cpp  ——  Arduino 框架入口（桌面 + App 框架相机系统）
 * ---------------------------------------------------------------------
 * 初始化顺序：屏幕 → SD → 自检画面 → FreeRTOS 任务（key/display/photoWrite）
 *             → Web 服务
 * 摄像头按需启动：开机不初始化，进入相机 App 才 init + 启动采集任务，
 * 退出相机 App 即 deinit 省电（桌面 + App 框架 Phase 1）。
 * =====================================================================
 */
#include "Arduino.h"
#include "config.h"
#include "camera_app.h"
#include "lcd_app.h"
#include "sd_app.h"
#include "rtos_app.h"
#include "web_app.h"
#include "ui_app.h"

void setup()
{
    Serial.begin(115200);
    Serial.setTxBufferSize(8192);   /* 加大 CDC TX 环形缓冲：高速打印不丢字符（截图 dump 用） */
    delay(300);
    Serial.println();
    Serial.println("================ ESP32-S3 Camera System 启动 ================");

    /* 先点亮屏幕，后续状态直接显示在屏上 */
    lcd_app_init();

    bool sd_ok = sd_app_init();

    /* 开机自检画面：企鹅 splash + SD 状态（摄像头为按需启动，进 App 才 init） */
    lcd_app_show_boot_screen(true, sd_ok);
    Serial.println("[MAIN] 自检画面停留 3 秒后进入桌面...");
    lcd_app_hold_boot_screen(3000);

    ui_app_init();      /* App 框架状态复位 */
    rtos_app_init();    /* keyTask / displayTask(桌面) / photoWriteTask */
    web_app_init();     /* WiFi AP + Web 服务 */

    Serial.println("[MAIN] 初始化完成，进入桌面（相机 App 按需启动摄像头）");
}

void loop()
{
    web_app_handle();
    /* [单一读者原则] 串口读取已全部收敛到 keyTask 的 serialCmdPoll：
     * 此前这里也读 Serial（截图命令 S/D/F/T），与 serialCmdPoll 分食同一
     * FIFO，导致 "getphoto" 等多字符命令前缀被随机吃掉 */
}
