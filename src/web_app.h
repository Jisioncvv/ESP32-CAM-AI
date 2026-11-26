/*
 * =====================================================================
 * web_app.h  ——  Web 服务模块接口
 * ---------------------------------------------------------------------
 * WiFi AP + 同步 WebServer（不用 ESPAsyncWebServer）
 * =====================================================================
 */
#pragma once
#include <Arduino.h>

/* 初始化 WebServer 路由（WiFi 默认关闭，按需在设置 App 的 WiFi 界面开启） */
void web_app_init(void);

/* 处理一次 HTTP 请求（应放在 Arduino loop() 中调用） */
void web_app_handle(void);

/* WiFi 热点开关（TOP 长按触发）：省电/防热，再按恢复 AP */
void web_app_toggle_wifi(void);

/* STA 模式：读 SD 卡 /wifi.txt 热点列表轮流连接（串口 wifista 触发），
 * 成功打印设备 IP；全部失败回退 AP 192.168.4.1 */
bool web_app_connect_sta(void);

/* STA 连接单个热点（设置 App WiFi 界面用）：阻塞至成功或超时，
 * 成功打印设备 IP 并置 g_wifiState=1，失败返回 false */
bool web_app_connect_ssid(const String& ssid, const String& pass, uint32_t timeoutMs);

/* 当前 WiFi 状态简述（设置页 WiFi 行显示）：IP / AP 192.168.4.1 / OFF */
String web_app_wifi_status_text(void);
bool web_app_wifi_busy(void);   /* WiFi 正在连接（照片流程避让：降 UXGA） */
void web_app_wifi_user_toggle(void);   /* 主界面 WiFi 总开关（开=STA 自动连记忆） */
void web_app_stream_hold(bool on);     /* 设备 QSXGA 拍照时暂停取景流（让出摄像头） */
