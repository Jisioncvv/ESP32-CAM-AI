/*
 * =====================================================================
 * rtos_app.cpp  ——  FreeRTOS 资源创建 / 任务 / 中断（精简版）
 * ---------------------------------------------------------------------
 * 任务结构（对齐官方 EDA-Camera 工程）：
 *   cameraTask  = 官方 cameraTask（fb_get → 队列）
 *   keyTask     = 官方按键状态机（单击/双击/长按，10ms 轮询）
 *   displayTask = 官方 displayTask（预览/相册切换，见 lcd_app.cpp）
 * 保留核心 FreeRTOS 知识点注释（[知识点N]）
 * =====================================================================
 */
#include "config.h"
#include "camera_app.h"
#include "lcd_app.h"
#include "web_app.h"
#include "sd_app.h"
#include "ui_app.h"
#include <SD.h>
#include <WiFi.h>

/* --------------------- 全局对象定义 --------------------- */
QueueHandle_t       g_frameQueue     = nullptr;   // [知识点2] 帧队列
SemaphoreHandle_t   g_cameraMutex    = nullptr;   // [知识点4] 摄像头互斥（官方 camMutex）
TaskHandle_t        g_cameraTaskHandle  = nullptr;
TaskHandle_t        g_keyTaskHandle     = nullptr;
TaskHandle_t        g_displayTaskHandle = nullptr;
TaskHandle_t        g_photoWriteTaskHandle = nullptr;

/* --------------------- 官方同款全局按键/显示状态 --------------------- */
volatile int g_btTOPstate   = 0;   /* 官方 btTOPstate */
volatile int g_btDownstate  = 0;   /* 官方 btDownstate */
volatile int  g_galleryView     = 0;   /* 0=相册列表 1=全屏查看 */
volatile bool g_galleryOpenReq  = false;
volatile bool g_galleryDeleteReq = false;   /* CAM 双击：全屏态删除当前照片 */
volatile bool g_galleryListRefreshReq = false;  /* 全屏→列表 刷新请求 */
volatile bool g_dispReqRotate    = false;   /* TOP 双击：预览旋转 180° */
volatile int8_t g_wifiState      = -1;      /* WiFi 状态：-1=初始 0=关 1=开（屏幕指示用） */

/* ===================================================================== */
/* cameraTask：官方 cameraTask 1:1（队列满丢帧）                            */
/* ===================================================================== */
void cameraTask(void* arg)
{
    (void)arg;
    static uint32_t s_capN = 0;
    static uint32_t s_capT0 = millis();
    while (true) {
        /* [崩溃防护] 驱动不在就绪态时绝不 fb_get：拍照流程 deinit 期间
         * 或恢复失败时，ll_cam_dma_reset 会解引用空的 DMA 通道直接 panic。
         * 这里退让等待，重启任务前 g_cameraReady 会被置 true */
        if (!g_cameraReady) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        uint32_t tF0 = micros();
        camera_fb_t* fb = esp_camera_fb_get();
        if (!fb) {
            Serial.println("Camera capture failed");
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        /* [在线切换守卫] 拍照/预览分辨率切换期间可能残留 QSXGA 大帧，
         * 非预览尺寸（JPEG QVGA 或回退模式 RGB565 QVGA）帧直接归还驱动 */
        if ((fb->format != PIXFORMAT_RGB565 && fb->format != PIXFORMAT_JPEG) ||
            fb->width > 320 || fb->height > 240) {
            esp_camera_fb_return(fb);
            continue;
        }
        uint32_t waitUs = micros() - tF0;
        s_capN++;
        uint32_t dt = millis() - s_capT0;
        if (dt >= 2000) {
#if SERIAL_PERF
            Serial.printf("[CAM] 采集 %lu 帧/2s（%.1f fps，fb_get 阻塞 %.1fms）\n",
                          (unsigned long)s_capN, s_capN * 1000.0f / dt,
                          (s_capN ? waitUs / 1000.0f : 0));
#endif
            s_capN = 0;
            s_capT0 = millis();
        }

        // [知识点2] 帧队列入队（官方深度1：满了直接丢弃本帧）
        if (xQueueSend(g_frameQueue, &fb, 0) != pdTRUE) {
            esp_camera_fb_return(fb);
        }
        /* 不加额外延时：fb_get 本身阻塞到下一帧就绪，
         * 任何额外节拍都会直接降低预览产能 */
        vTaskDelay(1);   /* 仅让出 CPU */
    }
}

/* ===================================================================== */
/* 官方按键状态机（keyTask.cpp 1:1 移植）                                    */
/* ------------------------------------------------------------------- */
#define DOUBLE_CLICK_THRESHOLD 400  /* 双击最大间隔 ms */
#define LONG_PRESS_THRESHOLD   800  /* 长按判断阈值 ms */
#define DEBOUNCE_DELAY         100  /* 去抖动时间 ms */

enum KeyState { IDLE, PRESSED, WAIT_SECOND_PRESS, LONG_PRESSED };

struct KeyInfo {
    int pin;
    volatile bool flag;
    KeyState state;
    unsigned long pressStart;
    unsigned long lastPress;
    bool skipNextSingle;
};

static KeyInfo keys[4] = {
    {PIN_KEY_CAM,  false, IDLE, 0, 0, true},   /* 拍照 */
    {PIN_KEY_TOP,  false, IDLE, 0, 0, true},   /* 上键 */
    {PIN_KEY_MID,  false, IDLE, 0, 0, true},   /* 中键 */
    {PIN_KEY_DOWN, false, IDLE, 0, 0, true},   /* 下键 */
};

/* 官方 IRAM 中断回调：置标志 */
static void IRAM_ATTR onKeyCamPress()  { keys[0].flag = true; }
static void IRAM_ATTR onKeyTopPress()  { keys[1].flag = true; }
static void IRAM_ATTR onKeyMidPress()  { keys[2].flag = true; }
static void IRAM_ATTR onKeyDownPress() { keys[3].flag = true; }

/* ---------- 官方回调语义（CAM 键按 App 模式分派） ---------- */
/* 拍照会删任务/反初始化摄像头，必须与 Web 端用同一把互斥锁串行化 */
static void camPhotoSaveSafe() {
    /* 摄像头未就绪（相册 App 全屏拍照等场景）：临时初始化，拍完断电 */
    bool temp = false;
    if (!g_cameraReady) {
        if (!camera_app_init()) return;
        temp = true;
    }
    xSemaphoreTake(g_cameraMutex, portMAX_DELAY);
    camera_app_photo_save(nullptr);
    xSemaphoreGive(g_cameraMutex);
    /* 临时启动的摄像头：相机 App 内 photo_save 已恢复预览并重启采集任务，
     * 绝不能在这里再 deinit（否则采集任务 fb_get 已释放驱动 → 崩溃重启）。
     * 只有非相机 App（无需预览）才断电 */
    if (temp && g_appMode != MODE_CAMERA) {
        esp_camera_deinit();
        g_cameraReady = false;
    }
}

static void camSingleClick() {
    switch (g_appMode) {
        case MODE_DESKTOP:                 /* 桌面：打开选中 App */
            ui_app_open(g_appSel);
            break;
        case MODE_CAMERA:                  /* 相机 App：官方单击拍照 */
            camPhotoSaveSafe();
            break;
        case MODE_GALLERY:                 /* 相册列表态：打开选中照片 */
            if (g_galleryView == 0) g_galleryOpenReq = true;
            /* 全屏态单击不动作（摄像头未运行，删除用双击） */
            break;
        case MODE_GAME:                    /* 游戏（俄罗斯方块）：CAM = 旋转 */
            g_gameDirReq = 3;
            break;
        case MODE_NOTES:                   /* 备忘：CAM = 删除当前选中 */
            g_noteDeleteReq = true;
            break;
        case MODE_SETTINGS:                /* 设置 App：CAM = 打开 WiFi 管理界面 */
            g_wifiUiOpenReq = true;
            break;
        default:                           /* 占位 App：任意 CAM 键退出 */
            g_appExitReq = true;
            break;
    }
}
static void camDoubleClick() {
    /* 全屏查看态：CAM 双击 = 删除当前照片（删除后回归相册列表） */
    if (g_appMode == MODE_GALLERY && g_galleryView == 1) {
        g_galleryDeleteReq = true;
        return;
    }
    /* 闪光灯拍照仅限相机 App 实时预览态 */
    if (g_appMode == MODE_CAMERA) {
        digitalWrite(PIN_FLASH_LED, LOW);
        camPhotoSaveSafe();
        digitalWrite(PIN_FLASH_LED, HIGH);
    }
}
static void camLongPress() {
    if (g_appMode == MODE_GAME) {              /* 游戏（俄罗斯方块）：CAM 长按 = 硬降 */
        g_gameDirReq = 4;
        return;
    }
    /* 闪光灯拍照仅限相机 App 实时预览态 */
    if (g_appMode == MODE_CAMERA) {
        digitalWrite(PIN_FLASH_LED, LOW);
        camPhotoSaveSafe();
        digitalWrite(PIN_FLASH_LED, HIGH);
    }
}
static void topSingleClick() {
    if (g_appMode == MODE_DESKTOP) {          /* 桌面：轮播上一个（循环） */
        g_appSel = (g_appSel + APP_COUNT - 1) % APP_COUNT;
        return;
    }
    if (g_appMode == MODE_GAME) {             /* 游戏（俄罗斯方块）：TOP = 左移 */
        g_gameDirReq = 1;
        return;
    }
    g_btTOPstate = 1;                         /* App 内：交给各自循环消费 */
}
static void downSingleClick() {
    if (g_appMode == MODE_DESKTOP) {          /* 桌面：轮播下一个（循环） */
        g_appSel = (g_appSel + 1) % APP_COUNT;
        return;
    }
    if (g_appMode == MODE_GAME) {             /* 游戏（俄罗斯方块）：DOWN = 右移 */
        g_gameDirReq = 2;
        return;
    }
    g_btDownstate = 1;
}
static void midSingleClick() {
    if (g_appMode == MODE_CAMERA) {           /* 相机 App：退出（关摄像头省电） */
        g_appExitReq = true;
    } else if (g_appMode == MODE_GALLERY) {   /* 相册：全屏→列表，列表→桌面 */
        if (g_galleryView == 1) {
            g_galleryView = 0;
            g_galleryListRefreshReq = true;
        } else {
            g_appExitReq = true;
        }
    } else if (g_appMode != MODE_DESKTOP) {   /* 占位 App：退出回桌面 */
        g_appExitReq = true;
    }
}
static void topDoubleClick()  { if (g_appMode == MODE_CAMERA) g_dispReqRotate = true; }  /* 预览旋转 180° */
static void topLongPress()
{   /* [主界面 WiFi 总开关] 像手机：开=STA 自动连记忆热点（后台任务，不卡按键），
     * 关=WiFi 全关省电。桌面与相机 App 均可用 */
    web_app_wifi_user_toggle();
}
static void midLongPress()    { ESP.restart(); }            /* 系统重启（卡死自救） */
static void downDoubleClick() { if (g_appMode == MODE_CAMERA) camera_app_set_gray(-1); }  /* 黑白/彩色切换 */

/* 官方 handleKey 状态机 1:1 */
static void handleKey(KeyInfo& key, const char* name,
                      void (*singleClick)(), void (*doubleClick)(), void (*longPress)())
{
    unsigned long now = millis();

    if (key.flag) {
        key.flag = false;

        if (key.state == IDLE) {
            key.pressStart = now;
            key.state = PRESSED;
            key.skipNextSingle = false;
        } else if (key.state == WAIT_SECOND_PRESS &&
                   (now - key.lastPress) < DOUBLE_CLICK_THRESHOLD) {
            if (doubleClick) doubleClick();
            Serial.printf("[KEY] %s 双击\n", name);
            key.state = IDLE;
            key.skipNextSingle = true;
        }
    }

    /* 长按触发改为"松手生效"：按住超过阈值只标记 LONG_PRESSED，
     * 松开瞬间才执行回调——避免按住过程中误触发（CAM 长按闪光灯体验差） */
    if (key.state == PRESSED && (now - key.pressStart > LONG_PRESS_THRESHOLD)) {
        key.state = LONG_PRESSED;
        key.skipNextSingle = true;
    }

    if ((key.state == PRESSED || key.state == LONG_PRESSED) &&
        digitalRead(key.pin) == HIGH) {
        if (key.state == PRESSED) {
            key.lastPress = now;
            key.state = WAIT_SECOND_PRESS;
        } else {
            /* 长按后松开：此刻执行长按回调 */
            if (longPress) longPress();
            Serial.printf("[KEY] %s 长按\n", name);
            key.state = IDLE;
        }
    }

    if (key.state == WAIT_SECOND_PRESS && (now - key.lastPress > DOUBLE_CLICK_THRESHOLD)) {
        if (!key.skipNextSingle && singleClick) {
            singleClick();
            Serial.printf("[KEY] %s 单击\n", name);
        }
        key.state = IDLE;
    }
}

/* ===================================================================== */
/* keyTask：核1，优先级3（官方最高优先级，拍照在其中执行）                     */
/* ===================================================================== */
/* 串口调试命令（USB 串口输入，keyTask 轮询）
 * [单一读者原则] 此前 main.cpp loop() 也读串口（截图命令），两个任务分食
 * 同一 FIFO，实测 "getphoto 1" 前缀被随机吃掉（收到 "oto 1"/"to 1"）。
 * 现在所有串口读取统一收敛到 serialCmdPoll */
static void serialCmdPoll(void)
{
    static String cmd;
    while (Serial.available()) {
        char c = (char)Serial.read();
        /* 单字符截图命令（无换行立即执行，兼容 serial_shot.ps1 裸字符发送）：
         * S=测试图案 D=完整桌面 F=仅白底 T=白底+瓷片 */
        if (cmd.length() == 0 && (c == 'S' || c == 'D' || c == 'F' || c == 'T')) {
            ui_app_serial_screenshot(c == 'S' ? 0 : c == 'D' ? 1 : c == 'F' ? 2 : 3);
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (cmd == "clearall") {
                int n = sd_app_clear_all();
                lcd_app_mark_gallery_dirty();
                Serial.printf("[CMD] clearall 完成：删除 %d 张\n", n);
            } else if (cmd == "reboot") {
                Serial.println("[CMD] 重启...");
                vTaskDelay(pdMS_TO_TICKS(100));
                ESP.restart();
            } else if (cmd == "sdinit") {
                /* 在线重试 SD 挂载（开机 init 失败/卡后插上的场景） */
                Serial.println("[CMD] 重新初始化 SD...");
                sd_app_init();
            } else if (cmd == "wifiscan") {
                /* 诊断：扫描周围 WiFi（ESP32 只见 2.4GHz），核对热点是否可见 */
                Serial.println("[CMD] 扫描 2.4GHz WiFi...");
                WiFi.mode(WIFI_STA);
                int n = WiFi.scanNetworks();
                for (int i = 0; i < n; ++i)
                    Serial.printf("[CMD] %-24s ch%02d %4ddBm enc=%d\n",
                                  WiFi.SSID(i).c_str(), WiFi.channel(i),
                                  WiFi.RSSI(i), WiFi.encryptionType(i));
                if (n <= 0) Serial.println("[CMD] 没扫到任何网络");
                WiFi.scanDelete();
            } else if (cmd == "wifista") {
                /* STA 模式：按 /wifi.txt 轮流连接电脑/手机热点（Web 直连取照片） */
                web_app_connect_sta();
            } else if (cmd == "wifistat") {
                /* 诊断：当前 WiFi 模式/状态/IP 一句话输出 */
                Serial.printf("[CMD] WiFi: %s（mode=%d）\n",
                              web_app_wifi_status_text().c_str(),
                              (int)WiFi.getMode());
            } else if (cmd.startsWith("wificonnect ")) {
                /* wificonnect SSID 密码：直连指定热点（绕过 /wifi.txt，测试用） */
                String rest = cmd.substring(12);
                int sp = rest.indexOf(' ');
                if (sp <= 0) {
                    Serial.println("[CMD] 用法: wificonnect SSID 密码");
                } else {
                    web_app_connect_ssid(rest.substring(0, sp),
                                         rest.substring(sp + 1), 20000);
                }
            } else if (cmd.startsWith("wifidel ")) {
                /* wifidel SSID：从 /wifi.txt 删除同名条目 */
                String ssid = cmd.substring(8);
                ssid.trim();
                File wf = SD.open("/wifi.txt", FILE_READ);
                String keep = "";
                int n2 = 0;
                if (wf) {
                    while (wf.available()) {
                        String l = wf.readStringUntil('\n');
                        l.trim();
                        if (!l.length()) continue;
                        int p2 = l.indexOf(' ');
                        if (p2 > 0 && l.substring(0, p2) == ssid) {
                            Serial.printf("[CMD] 已删除: %s\n", l.c_str());
                            continue;
                        }
                        keep += l + "\n";
                        ++n2;
                    }
                    wf.close();
                }
                SD.remove("/wifi.txt");
                File w2 = SD.open("/wifi.txt", FILE_WRITE);
                if (w2) { w2.print(keep); w2.close(); }
                Serial.printf("[CMD] /wifi.txt 剩余 %d 条\n", n2);
            } else if (cmd.startsWith("wifiadd ")) {
                /* wifiadd SSID 密码：追加到 /wifi.txt（密码可含空格，取剩余全部） */
                String rest = cmd.substring(8);
                int sp = rest.indexOf(' ');
                if (sp <= 0) {
                    Serial.println("[CMD] 用法: wifiadd SSID 密码");
                } else {
                    File wf = SD.open("/wifi.txt", FILE_APPEND);
                    if (wf) {
                        wf.println(rest);
                        wf.close();
                        Serial.printf("[CMD] 已存热点: %s\n", rest.substring(0, sp).c_str());
                    } else {
                        Serial.println("[CMD] /wifi.txt 写入失败");
                    }
                }
            } else if (cmd == "wifilist") {
                File wf = SD.open("/wifi.txt", FILE_READ);
                if (!wf) {
                    Serial.println("[CMD] /wifi.txt 不存在");
                } else {
                    int i = 0;
                    while (wf.available()) {
                        String line = wf.readStringUntil('\n');
                        line.trim();
                        if (line.length())
                            Serial.printf("[CMD] %d: %s\n", ++i, line.c_str());
                    }
                    wf.close();
                }
            } else if (cmd.startsWith("ae ") || cmd.startsWith("wb ") || cmd.startsWith("sat ")) {
                /* 实时调参（免重烧验证）：ae -2..2 曝光目标 / wb 0..4 白平衡
                 * 模式(0自动 1晴天 2阴天 3水 4白炽灯) / sat -2..2 饱和度 */
                if (!g_cameraReady) {
                    Serial.println("[CMD] 摄像头未启动（先进相机 App）");
                } else {
                    sensor_t* s = esp_camera_sensor_get();
                    int v = cmd.substring(3).toInt();
                    String w = cmd.substring(0, 2);
                    if (v < -2) v = -2;
                    if (v > (w == "wb" ? 4 : 2)) v = (w == "wb" ? 4 : 2);
                    if (w == "ae") { s->set_ae_level(s, v); Serial.printf("[CMD] ae_level=%d\n", v); }
                    if (w == "wb") { s->set_wb_mode(s, v); Serial.printf("[CMD] wb_mode=%d\n", v); }
                    if (w == "sat") { s->set_saturation(s, v); Serial.printf("[CMD] saturation=%d\n", v); }
                }
            } else if (cmd == "snap") {
                /* 远程拍照（A/B 对比调参用）：存卡后用 getphoto/Web 回读 */
                if (g_appMode == MODE_CAMERA) {
                    camPhotoSaveSafe();
                } else {
                    Serial.println("[CMD] 需在相机 App 预览态");
                }
            } else if (cmd == "wifioff") {
                /* 调试：彻底关 WiFi（验证拍照速度是否恢复历史 2~3s） */
                WiFi.disconnect(true, true);
                WiFi.mode(WIFI_OFF);
                Serial.println("[CMD] WiFi 已关闭");
            } else if (cmd == "wifion") {
                web_app_wifi_user_toggle();
                Serial.println("[CMD] WiFi 开启请求（后台自动连）");
            } else if (cmd.startsWith("psize")) {
                /* psize N（sensor.h framesize_t 枚举）：9=SVGA 10=XGA 11=HD
                 * 12=SXGA 13=UXGA 19=QSXGA（设备拍照分辨率实测用） */
                int n = cmd.substring(5).toInt();
                if (n >= 0 && n <= 19) {
                    g_photoFs = n;
                    Serial.printf("[CMD] 拍照分辨率 → framesize=%d\n", n);
                } else Serial.println("[CMD] 用法 psize 0..19");
            } else if (cmd == "appcam") {
                /* 调试：切到相机 App（预览跑起来后可用 snap 测真实按键拍照耗时） */
                Serial.println("[CMD] 切换相机 App");
                g_appMode = MODE_CAMERA;
                g_appExitReq = false;
            } else if (cmd.startsWith("del ")) {
                /* del N：删除 photo_N.jpg + photo_Ns.jpg（清理测试照片用） */
                int n = cmd.substring(4).toInt();
                sd_app_delete_number(n);
            } else if (cmd == "gstat") {
                lcd_app_gallery_debug_dump();
            } else if (cmd == "gshot") {
                lcd_app_gallery_debug_shot();
            } else if (cmd == "appgal") {
                Serial.println("[CMD] 切换相册 App");
                g_appMode = MODE_GALLERY;
                g_appExitReq = false;
            } else if (cmd.startsWith("gdn ")) {
                int n = cmd.substring(4).toInt();
                for (int i = 0; i < n; ++i) {
                    g_btDownstate = 1;
                    vTaskDelay(pdMS_TO_TICKS(260));
                }
            } else if (cmd.startsWith("gup ")) {
                int n = cmd.substring(4).toInt();
                for (int i = 0; i < n; ++i) {
                    g_btTOPstate = 1;
                    vTaskDelay(pdMS_TO_TICKS(260));
                }
            } else if (cmd.startsWith("getsm ")) {
                /* getsm N：回传第 N 新的小图 photo_Ns.jpg（诊断缩略图花屏） */
                int idx = cmd.substring(6).toInt();
                if (idx < 1) idx = 1;
                Serial.printf("[CMD] getsm %d 开始回传...\n", idx);
                sd_app_dump_photo(idx, true);
            } else if (cmd.startsWith("getphoto")) {
                /* getphoto N：回传第 N 新的照片（配合 tools/fetch_photos.py） */
                int idx = cmd.substring(8).toInt();
                if (idx < 1) idx = 1;
                Serial.printf("[CMD] getphoto %d 开始回传...\n", idx);
                sd_app_dump_photo(idx);
            } else if (cmd.length()) {
                Serial.printf("[CMD] 未知命令: %s\n", cmd.c_str());
            }
            cmd = "";
        } else if (cmd.length() < 64) {
            cmd += c;
        }
    }
}

void keyTask(void* arg)
{
    (void)arg;
    for (int i = 0; i < 4; ++i) {
        pinMode(keys[i].pin, INPUT_PULLUP);
    }
    pinMode(PIN_FLASH_LED, OUTPUT);
    digitalWrite(PIN_FLASH_LED, HIGH);   /* 闪光灯灭（低电平亮） */

    /* 官方：4 路独立下降沿中断置标志 */
    attachInterrupt(digitalPinToInterrupt(PIN_KEY_CAM),  onKeyCamPress,  FALLING);
    attachInterrupt(digitalPinToInterrupt(PIN_KEY_TOP),  onKeyTopPress,  FALLING);
    attachInterrupt(digitalPinToInterrupt(PIN_KEY_MID),  onKeyMidPress,  FALLING);
    attachInterrupt(digitalPinToInterrupt(PIN_KEY_DOWN), onKeyDownPress, FALLING);

    while (true) {
        handleKey(keys[0], "拍照键", camSingleClick, camDoubleClick, camLongPress);
        handleKey(keys[1], "上键",   topSingleClick, topDoubleClick, topLongPress);
        handleKey(keys[2], "中键",   midSingleClick, nullptr, midLongPress);
        handleKey(keys[3], "下键",   downSingleClick, downDoubleClick, nullptr);
        serialCmdPoll();             /* USB 串口调试命令 */

        vTaskDelay(pdMS_TO_TICKS(10));   /* 官方 10ms 轮询 */
    }
}

/* ===================================================================== */
/* rtos_restart_camera_task —— 官方 PhotoSave 重建采集任务（按需启动）       */
/* ===================================================================== */
void rtos_restart_camera_task(void)
{
    if (g_cameraTaskHandle != NULL) return;
    xTaskCreatePinnedToCore(cameraTask, "cam", TASK_CAMERA_STACK,
                            nullptr, TASK_CAMERA_PRIO, &g_cameraTaskHandle,
                            TASK_CAMERA_CORE);
}

/* ===================================================================== */
/* rtos_stop_camera_task —— 停止采集任务（相机 App 退出/拍照流程前置步骤）   */
/* ===================================================================== */
void rtos_stop_camera_task(void)
{
    if (g_cameraTaskHandle != NULL) {
        vTaskDelete(g_cameraTaskHandle);
        g_cameraTaskHandle = NULL;
        /* 等待 DMA/相机控制器完全释放（官方同款，非常关键） */
        vTaskDelay(pdMS_TO_TICKS(30));
        /* 清空帧队列：否则 deinit 释放帧缓冲后，队列里残留的 camera_fb_t*
         * 就是悬空指针，再取到会读写已释放内存 → 画面卡死/崩溃 */
        if (g_frameQueue) xQueueReset(g_frameQueue);
    }
}

/* ===================================================================== */
/* rtos_app_init —— 创建所有 FreeRTOS 资源                                 */
/* （cameraTask 按需启动：进入相机 App 才由 ui_app_open 创建）              */
/* ===================================================================== */
void rtos_app_init(void)
{
    // [知识点2] 帧队列：camera_fb_t*（官方深度 1）
    g_frameQueue = xQueueCreate(FRAME_QUEUE_LEN, sizeof(camera_fb_t*));

    // [知识点4] 摄像头硬件互斥量（官方 camMutex，按键拍照与 Web 拍照并发保护）
    g_cameraMutex = xSemaphoreCreateMutex();

    // [知识点1] xTaskCreatePinnedToCore：创建并绑核任务
    xTaskCreatePinnedToCore(keyTask,    "key",  TASK_KEY_STACK,
                            nullptr, TASK_KEY_PRIO,    &g_keyTaskHandle,    TASK_KEY_CORE);
    xTaskCreatePinnedToCore(displayTask, "disp", TASK_DISPLAY_STACK,
                            nullptr, TASK_DISPLAY_PRIO, &g_displayTaskHandle, TASK_DISPLAY_CORE);
    xTaskCreatePinnedToCore(photoWriteTask, "sdw", TASK_PHWRITE_STACK,
                            nullptr, TASK_PHWRITE_PRIO, &g_photoWriteTaskHandle, TASK_PHWRITE_CORE);

    Serial.println("[RTOS] 3 任务 + 1 队列 + 1 互斥锁 已创建（cameraTask 按需启动）");
}
