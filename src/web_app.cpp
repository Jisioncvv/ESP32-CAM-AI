/*
 * =====================================================================
 * web_app.cpp  ——  WiFi AP + 同步 WebServer（新版 UI + 非阻塞取帧）
 * ---------------------------------------------------------------------
 * SSID="ESP32-CAM" / 密码="12345678" / IP=192.168.4.1
 * 路由：
 *   GET  /                首页（fetch+blob 轮询 /snap，~12fps，自动测速）
 *   GET  /snap            单张 JPEG（非阻塞：无帧返回 204，绝不拖住浏览器轮询）
 *   GET  /status          JSON 状态（fps/模式/设备状态）
 *   GET  /capture         拍照并保存 TF 卡（同步，互斥锁保护）
 *   GET  /photos          相册（网格 + 返回首页按钮）
 *   GET  /view?name=      预览
 *   GET  /download?name=  下载
 *   GET  /delete?name=    删除后跳回相册
 * =====================================================================
 */
#include "config.h"
#include "camera_app.h"
#include "sd_app.h"
#include "lcd_app.h"
#include "web_app.h"
#include "ui_app.h"
#include <WiFi.h>
#include <WebServer.h>
#include <SD.h>

static WebServer server(80);

/* [教训] WiFi OFF 状态下 server.begin() 会触发 lwIP 断言崩溃
 * （tcpip_send_msg_wait_sem Invalid mbox）：tcpip 线程未启动。
 * WebServer 改为"懒启动"——任何 WiFi 接口（AP/STA）上线后才 begin */
static bool s_serverStarted = false;
static void web_app_ensure_server(void)
{
    if (!s_serverStarted) {
        server.begin();
        s_serverStarted = true;
        Serial.println("[WEB] WebServer 启动（端口 80）");
    }
}

/* 摄像头按需启动：未就绪时临时初始化（Web 拍照场景，拍完由调用者断电）。
 * [修复] 直接用 JPEG 拍照配置：旧流程临时 init 预览配置后 photo_save
 * 又立刻 deinit 重启 JPEG 配置，1s 内猛切会挂驱动（fb_get 卡 ~25s） */
static bool web_ensure_camera(bool* tempStarted)
{
    *tempStarted = false;
    if (g_cameraReady) return true;
    /* 直接 QSXGA 拍照配置（连接中才 UXGA）：若 init UXGA 后 photo_save
     * 又立刻重建 QSXGA = 1s 内猛切，会挂驱动。档位与 photo_save 目标一致 */
    if (!camera_app_init_jpeg_ex(web_app_wifi_busy() ? FRAMESIZE_UXGA
                                                     : FRAMESIZE_QSXGA))
        return false;
    *tempStarted = true;
    Serial.println("[WEB] 摄像头临时启动（JPEG 拍照配置，桌面/相册态 Web 拍照）");
    return true;
}

/* 临时启动的摄像头用完断电（photo_save 后台版不再自动断电非活跃摄像头） */
static void web_camera_release(bool tempStarted)
{
    if (tempStarted) {
        esp_camera_deinit();
        g_cameraReady = false;
        Serial.println("[WEB] 摄像头临时断电");
    }
}

/* ============================ 首页 HTML ============================
 * 现代风格：径向渐变背景 + 玻璃拟态卡片 + 实时状态徽章 */
static const char* kIndexHTML =
"<!DOCTYPE html><html lang='zh'><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>ESP32-CAM 控制台</title>"
"<style>"
"*{box-sizing:border-box}"
":root{--txt:#e8edf4;--sub:#94a3b8;--acc:#38bdf8;--line:rgba(148,163,184,.16);--card:rgba(17,24,36,.72)}"
"body{font-family:'Segoe UI',system-ui,sans-serif;color:var(--txt);margin:0;padding:20px 16px 48px;"
"background:#0a0e14;"
"background-image:radial-gradient(640px 340px at 12% -8%,rgba(56,189,248,.16),transparent 62%),"
"radial-gradient(520px 320px at 92% -4%,rgba(129,140,248,.14),transparent 62%)}"
".wrap{max-width:980px;margin:0 auto}"
"h1{font-size:1.4em;display:flex;align-items:center;gap:12px;margin:6px 0 4px;letter-spacing:.3px}"
"h1 .logo{width:34px;height:34px;border-radius:10px;display:grid;place-items:center;font-size:20px;"
"background:linear-gradient(135deg,#1f6feb,#38bdf8);box-shadow:0 4px 14px rgba(56,189,248,.35)}"
"h1 small{color:var(--sub);font-weight:400;font-size:.55em;margin-left:2px}"
".sub{color:var(--sub);font-size:.85em;margin:0 0 16px 46px}"
".chips{display:flex;gap:10px;flex-wrap:wrap;margin-bottom:16px}"
".chip{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:8px 14px;font-size:.85em;"
"color:var(--sub);backdrop-filter:blur(8px);display:flex;align-items:center;gap:8px}"
".chip b{color:var(--txt);font-family:Consolas,monospace}"
".chip .i{font-size:1.05em}"
".grid{display:grid;grid-template-columns:1fr;gap:16px}"
"@media(min-width:760px){.grid{grid-template-columns:2fr 1fr}}"
".panel{position:relative;background:var(--card);border:1px solid var(--line);border-radius:16px;overflow:hidden;"
"backdrop-filter:blur(10px);box-shadow:0 8px 30px rgba(2,6,16,.45)}"
"#view{width:100%;display:block;background:#000;aspect-ratio:4/3;object-fit:contain}"
".bar{display:flex;gap:9px;padding:12px;flex-wrap:wrap;border-top:1px solid var(--line)}"
"button{border:0;border-radius:10px;padding:10px 16px;font-size:.9em;cursor:pointer;color:#fff;"
"transition:.18s;background:#1f6feb;font-weight:600}"
"button:hover{transform:translateY(-1px);filter:brightness(1.12)}"
"button:active{transform:translateY(0)}"
"button.primary{background:linear-gradient(135deg,#1f6feb,#38bdf8);box-shadow:0 4px 14px rgba(56,189,248,.3)}"
"button.gray{background:rgba(148,163,184,.14);border:1px solid var(--line)}"
".paneltitle{padding:12px 16px 2px;font-size:.82em;color:var(--sub);letter-spacing:.5px}"
".stats{padding:8px 16px 14px;font-size:.9em;line-height:2.1}"
".stats div{display:flex;justify-content:space-between;border-bottom:1px dashed var(--line)}"
".stats div:last-child{border-bottom:0}"
".stats span{color:#7dd3fc;font-family:Consolas,monospace}"
".toast{position:fixed;left:50%;bottom:28px;transform:translateX(-50%);background:linear-gradient(135deg,#059669,#10b981);"
"color:#fff;padding:11px 20px;border-radius:12px;opacity:0;transition:.3s;pointer-events:none;max-width:92vw;"
"box-shadow:0 8px 24px rgba(16,185,129,.4);font-size:.92em}"
".toast.show{opacity:1}"
"a{color:#7dd3fc;text-decoration:none}"
".foot{text-align:center;color:var(--sub);font-size:.78em;margin-top:22px}"
"</style></head><body><div class='wrap'>"
"<h1><span class='logo'>📷</span>ESP32-S3 摄像头控制台<small>FreeRTOS Demo</small></h1>"
"<p class='sub'>局域网远程控制 · 同一 WiFi 下的手机 / 电脑均可访问</p>"
"<div class='chips'>"
"<span class='chip'><span class='i'>⚡</span>帧率 <b id='fps'>--</b> fps</span>"
"<span class='chip'><span class='i'>🧭</span>模式 <b id='mode'>--</b></span>"
"<span class='chip'><span class='i'>🎥</span>CAM <b id='cam'>--</b></span>"
"<span class='chip'><span class='i'>💾</span>SD <b id='sd'>--</b></span>"
"</div>"
"<div class='grid'>"
"<div class='panel'>"
"<div id='hint' style='position:absolute;left:50%;top:42%;transform:translate(-50%,-50%);color:var(--sub);font-size:.95em;text-align:center'>"
"⏳ 正在连接视频流（1 秒内出画面，之后实时刷新）...</div>"
"<img id='view' alt='等待画面...'>"
"<div class='bar'>"
"<button class='primary' onclick='doCapture()'>📸 拍照存卡</button>"
"<button onclick='snap()'>⚡ 高清快照</button>"
"<button class='gray' onclick='location.href=\"/photos\"'>🖼 相册</button>"
"<button class='gray' onclick='location.href=\"/notes\"'>📝 备忘</button>"
"</div></div>"
"<div class='panel'><div class='paneltitle'>DEVICE INFO</div><div class='stats'>"
"<div>设备 IP<span>192.168.50.131</span></div>"
"<div>照片命名<span>photo_N.jpg</span></div>"
"<div>按键 GPIO<span>10 / 9 / 8 / 7</span></div>"
"<div>闪光灯 GPIO<span>6（低有效）</span></div>"
"<div>无线模式<span>WiFi STA</span></div>"
"</div></div>"
"</div>"
"<div class='foot'>ESP32-S3 · OV5640 · ST7789 · SD — 单板相机系统</div>"
"<div class='toast' id='toast'></div>"
"<script>"
"const $=id=>document.getElementById(id);"
"let busy=false,pvBusy=false;"
"async function preview(){"
" if(pvBusy||busy||document.hidden){return}"
" pvBusy=true;"
" try{const r=await fetch('/preview?'+Date.now(),{cache:'no-store'});"
"  if(r.status==200){const b=await r.blob();"
"   if($('view').dataset.p)URL.revokeObjectURL($('view').dataset.p);"
"   $('view').src=URL.createObjectURL(b);$('view').dataset.p=$('view').src;"
"   $('hint').style.display='none'}"
"  else if(r.status==503){const t=await r.text();"
"   $('hint').textContent = t.indexOf('camera busy')>=0"
"    ? '📱 设备屏幕正在使用相机，退出相机 App 后自动恢复'"
"    : '⏳ WiFi 连接中，请稍候...';"
"   $('hint').style.display='block'}}"
" catch(e){}"
" pvBusy=false}"
"setInterval(preview,350);setTimeout(preview,200);"
"async function snap(){"
" if(busy){return}"
" busy=true;toast('截取当前画面存入相册...');"
" try{const r=await fetch('/snap',{cache:'no-store'});"
"  if(r.status==200){const b=await r.blob();"
"   if($('view').dataset.u)URL.revokeObjectURL($('view').dataset.u);"
"   $('view').src=URL.createObjectURL(b);$('view').dataset.u=$('view').src;"
"   toast('快照完成 ✓')}"
"  else{toast('快照失败 ('+r.status+')')}}"
" catch(e){toast('快照失败')} busy=false}"
"async function st(){try{const j=await(await fetch('/status')).json();"
" $('fps').textContent=j.fps;$('mode').textContent=j.mode;"
" $('cam').textContent=j.cam?'OK':'FAIL';$('sd').textContent=j.sd?'OK':'FAIL';"
" }catch(e){}}"
"setInterval(st,2000);st();"
"function toast(m){const t=$('toast');t.textContent=m;t.classList.add('show');"
" clearTimeout(t._h);t._h=setTimeout(()=>t.classList.remove('show'),2800)}"
"async function act(url){try{toast(await(await fetch(url)).text())}catch(e){toast('请求失败')}}"
"const doCapture=()=>act('/capture');"
"</script></div></body></html>";

/* ============================ 路由处理器 ============================ */

static void handleRoot() {
    server.send_P(200, "text/html", kIndexHTML);
}

/* 旧 /snap（QSXGA 完整拍照流程）已由数据流架构取代：见下方
 * handleSnap——网页拍照 = 截取当前 VGA 流帧，<1s 完成，无配置切换 */

/* =====================================================================
 * [数据流架构·用户拍板] 设备只推流，网页拍照 = 截取当前流帧存相册
 *   VGA JPEG 后台流（双缓冲），/preview 回现成帧（~0.2s/帧，网页 350ms
 *   轮询≈准直播）；/snap、/capture = 取最新流帧写 SD（<1s，不再有
 *   QSXGA 重初始化/WiFi 在线拍照那套复杂度）
 * =================================================================== */
#define STREAM_BUF_SIZE  (96 * 1024)     /* VGA q10 JPEG 实测 ~55KB，留余量 */

static uint8_t*  s_streamBuf[2] = { nullptr, nullptr };
static volatile size_t s_streamLen[2] = { 0, 0 };
static volatile int    s_streamRd    = 0;
static SemaphoreHandle_t s_streamMutex = nullptr;
static volatile bool    s_streamHold = false;   /* 设备 QSXGA 拍照时让路 */
void web_app_stream_hold(bool on) { s_streamHold = on; }

static void streamTask(void* pv)
{
    for (;;) {
        /* 让路：设备相机 App / 设备拍照 / 相机未就绪或非 JPEG 配置。
         * [花屏修复] 状态不满足时立即作废缓存帧——摄像头重建配置过渡期
         * 驱动会吐损坏帧，旧帧也早已过期，绝不能留给网页显示 */
        if (s_streamHold || g_appMode == MODE_CAMERA ||
            !g_cameraReady || !g_cameraPhotoCfg) {
            if (s_streamLen[0] | s_streamLen[1]) {
                xSemaphoreTake(s_streamMutex, portMAX_DELAY);
                s_streamLen[0] = 0;
                s_streamLen[1] = 0;
                xSemaphoreGive(s_streamMutex);
            }
            vTaskDelay(pdMS_TO_TICKS(120));
            continue;
        }
        xSemaphoreTake(g_cameraMutex, portMAX_DELAY);
        camera_fb_t* fb = esp_camera_fb_get();
        xSemaphoreGive(g_cameraMutex);
        /* [花屏修复] JPEG 帧完整性校验：帧头 FF D8 + 帧尾 FF D9。
         * 重建配置后首帧可能损坏（粉条纹花屏），不校验就会上网页 */
        bool jpegOk = fb && fb->len > 4 &&
                      fb->buf[0] == 0xFF && fb->buf[1] == 0xD8 &&
                      fb->buf[fb->len - 2] == 0xFF && fb->buf[fb->len - 1] == 0xD9;
        if (jpegOk && fb->len <= STREAM_BUF_SIZE) {
            int wr = s_streamRd ^ 1;
            xSemaphoreTake(s_streamMutex, portMAX_DELAY);
            memcpy(s_streamBuf[wr], fb->buf, fb->len);
            s_streamLen[wr] = fb->len;
            s_streamRd = wr;
            xSemaphoreGive(s_streamMutex);
        }
        if (fb) esp_camera_fb_return(fb);
        vTaskDelay(pdMS_TO_TICKS(30));   /* VGA 帧 ~50ms，流 ~5fps */
    }
}

/* 确保取景摄像头在跑（VGA JPEG）；返回 false=无法启动 */
static bool stream_ensure_camera(void)
{
    /* [关键修复] 设备拍照期间绝不允许碰摄像头：拍照流程刚 deinit 完，
     * 浏览器轮询进来发现"未就绪"就会重新 init VGA，与拍照的 QSXGA init
     * 撞车 → 两边 fb_get 全部超时（实测 19 秒拍空）。这是"拍照忽快忽慢、
     * 有时十几二十秒"的真凶 */
    if (s_streamHold) return false;
    if (web_app_wifi_busy()) return false;   /* WiFi 连接中不碰摄像头（会打挂自动连） */
    if (g_appMode == MODE_CAMERA) return false;
    if (!g_cameraReady) {
        if (!camera_app_init_jpeg_ex(FRAMESIZE_VGA)) return false;
        Serial.println("[WEB] 取景流摄像头启动（VGA JPEG）");
        return true;
    }
    if (!g_cameraPhotoCfg) {                 /* 设备拍照后残留预览配置 → 重建 VGA */
        esp_camera_deinit();
        g_cameraReady = false;
        return camera_app_init_jpeg_ex(FRAMESIZE_VGA);
    }
    return true;
}

/* 取最新完整流帧（阻塞最多 waitMs） */
static uint8_t* stream_get_latest(size_t* outLen, uint32_t waitMs)
{
    uint32_t t0 = millis();
    while (millis() - t0 < waitMs) {
        size_t len = s_streamLen[s_streamRd];
        if (len) { *outLen = len; return s_streamBuf[s_streamRd]; }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    *outLen = 0;
    return nullptr;
}

static void handlePreview() {
    if (s_streamHold) { server.send(503, "text/plain", "photo in progress"); return; }
    if (!stream_ensure_camera()) {
        server.send(503, "text/plain",
                    web_app_wifi_busy() ? "wifi connecting" : "camera busy on device");
        return;
    }
    size_t len = 0;
    uint8_t* buf = stream_get_latest(&len, 600);
    if (s_streamHold || !buf) { server.send(204, "text/plain", ""); return; }
    WiFiClient cl = server.client();
    cl.print("HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: "
             + String(len) + "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n");
    xSemaphoreTake(s_streamMutex, portMAX_DELAY);
    cl.write(buf, s_streamLen[s_streamRd]);
    xSemaphoreGive(s_streamMutex);
}

/* [网页拍照] 截取当前流帧 → 写 SD 相册（photo_N.jpg）。<1s 完成，
 * 画质 = 流档位 VGA 640x480。核心逻辑供 /snap（回图）与 /capture（回文本）共用 */
static uint8_t s_snapTmp[STREAM_BUF_SIZE];   /* 拷出缓冲（流任务可能立刻覆盖双缓冲） */
static bool stream_capture_save(String* pathOut, size_t* lenOut)
{
    if (!g_sdReady) return false;
    if (!stream_ensure_camera()) return false;
    size_t len = 0;
    uint8_t* buf = stream_get_latest(&len, 1000);
    if (!buf || !len) return false;
    xSemaphoreTake(s_streamMutex, portMAX_DELAY);
    memcpy(s_snapTmp, buf, len);
    xSemaphoreGive(s_streamMutex);
    String path = camera_app_next_photo_path();
    if (!sd_app_save_photo(path, s_snapTmp, len)) return false;
    lcd_app_mark_gallery_dirty();
    Serial.printf("[WEB] 流帧已存相册 %s（%uB）\n", path.c_str(), (unsigned)len);
    *pathOut = path;
    *lenOut = len;
    return true;
}

static void handleSnap() {
    String path; size_t len = 0;
    if (!stream_capture_save(&path, &len)) {
        server.send(503, "text/plain",
                    web_app_wifi_busy() ? "wifi connecting" : "capture failed");
        return;
    }
    WiFiClient cl = server.client();
    cl.print("HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: "
             + String(len) + "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n");
    cl.write(s_snapTmp, len);   /* 刚存入相册的那一帧 */
}

/* JSON 状态（供首页 2 秒轮询） */
static void handleStatus() {
    static const char* kModes[] = {"DESKTOP", "CAMERA", "GALLERY", "SETTINGS", "GAME", "NOTES"};
    int m = g_appMode;
    if (m < 0 || m > 5) m = 0;
    String j = "{\"fps\":";
    j += String(g_lcdFps, 1);
    j += ",\"mode\":\"";
    j += kModes[m];
    j += "\",\"cam\":";
    j += g_cameraReady ? "true" : "false";
    j += ",\"sd\":";
    j += g_sdReady ? "true" : "false";
    j += "}";
    server.send(200, "application/json", j);
}

/* 拍照存卡：数据流架构 = 截取当前 VGA 流帧写 SD（<1s）。
 * 原 QSXGA 完整拍照流程仅保留给设备按键拍照（相机 App 内） */
static void handleCapture() {
    String path; size_t len = 0;
    if (stream_capture_save(&path, &len))
        server.send(200, "text/plain", "OK: " + path);
    else
        server.send(503, "text/plain",
                    web_app_wifi_busy() ? "wifi connecting" : "capture failed");
}

static void handlePhotos() {
    String html =
"<!DOCTYPE html><html lang='zh'><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>相册</title><style>"
"*{box-sizing:border-box}"
":root{--txt:#e8edf4;--sub:#94a3b8;--line:rgba(148,163,184,.16);--card:rgba(17,24,36,.72)}"
"body{font-family:'Segoe UI',system-ui,sans-serif;color:var(--txt);padding:20px 16px 48px;margin:0;"
"background:#0a0e14;"
"background-image:radial-gradient(640px 340px at 12% -8%,rgba(56,189,248,.16),transparent 62%),"
"radial-gradient(520px 320px at 92% -4%,rgba(129,140,248,.14),transparent 62%)}"
".wrap{max-width:1080px;margin:0 auto}"
"h1{font-size:1.4em;display:flex;align-items:center;gap:12px;margin:6px 0 4px}"
"h1 .logo{width:34px;height:34px;border-radius:10px;display:grid;place-items:center;font-size:20px;"
"background:linear-gradient(135deg,#059669,#38bdf8);box-shadow:0 4px 14px rgba(56,189,248,.3)}"
"h1 .cnt{color:var(--sub);font-size:.5em;font-weight:400;margin-left:4px}"
".top{display:flex;gap:9px;margin:14px 0 18px;flex-wrap:wrap}"
"button,a.btn{background:rgba(148,163,184,.14);color:var(--txt);border:1px solid var(--line);"
"border-radius:10px;padding:10px 16px;cursor:pointer;text-decoration:none;font-size:.9em;transition:.18s;font-weight:600;display:inline-block}"
"a.btn.home{background:linear-gradient(135deg,#1f6feb,#38bdf8);border:0;color:#fff;"
"box-shadow:0 4px 14px rgba(56,189,248,.3)}"
"button:hover,a.btn:hover{transform:translateY(-1px);filter:brightness(1.12)}"
"button.danger,a.btn.rd{background:rgba(239,68,68,.16);border-color:rgba(239,68,68,.35);color:#fca5a5}"
".grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(230px,1fr));gap:16px}"
".card{background:var(--card);border:1px solid var(--line);border-radius:14px;overflow:hidden;"
"backdrop-filter:blur(8px);transition:.2s;box-shadow:0 6px 20px rgba(2,6,16,.4)}"
".card:hover{transform:translateY(-3px);border-color:rgba(56,189,248,.45);box-shadow:0 10px 28px rgba(56,189,248,.16)}"
".card .ph{position:relative;cursor:zoom-in}"
".card img{width:100%;aspect-ratio:4/3;object-fit:cover;display:block;background:#000}"
".card .badge{position:absolute;right:8px;top:8px;background:rgba(2,6,16,.72);border:1px solid var(--line);"
"color:#cbd5e1;border-radius:8px;padding:2px 8px;font-size:.72em;font-family:Consolas,monospace}"
".card .info{padding:9px 12px;font-size:.84em;display:flex;justify-content:space-between;align-items:center;gap:6px}"
".card .name{color:var(--txt);font-family:Consolas,monospace;font-size:.9em}"
".card a{color:#7dd3fc;text-decoration:none;font-size:.92em;margin-left:9px}"
".card a:hover{text-decoration:underline}"
".empty{color:var(--sub);text-align:center;padding:56px 20px;border:1px dashed var(--line);border-radius:16px}"
".empty .big{font-size:2.6em;margin-bottom:10px}"
".foot{text-align:center;color:var(--sub);font-size:.78em;margin-top:26px}"
/* 灯箱（点卡片全屏看大图） */
"#lb{display:none;position:fixed;inset:0;background:rgba(3,7,15,.94);z-index:60;backdrop-filter:blur(4px)}"
"#lb img{position:absolute;inset:0;margin:auto;max-width:94vw;max-height:86vh;object-fit:contain;border-radius:10px;"
"box-shadow:0 20px 60px rgba(0,0,0,.6)}"
"#lb .ops{position:fixed;top:14px;right:14px;display:flex;gap:9px}"
"#lb .ops a{font-size:.92em}"
"</style></head><body><div class='wrap'>"
"<h1><span class='logo'>🖼</span>相册<span class='cnt' id='cnt'></span></h1>"
"<div class='top'><a class='btn home' href='/'>← 控制台</a>"
"<button onclick='location.reload()'>🔄 刷新</button>"
"<button class='danger' onclick=\"if(confirm('确定清空相册？全部照片将被删除！'))"
"{location.href='/clearall'}\">🗑 清空</button></div>"
"<div class='grid'>";

    int count = 0;
    if (g_sdReady) {
        File root = SD.open("/");
        if (root) {
            /* 先收集（文件名+大小），倒序显示（最新的在前）
             * [修复] photo_Ns.jpg 是配对的缩略图不是独立照片，必须过滤，
             * 否则每张照片显示两张重复卡片 */
            String names[64];
            long  sizes[64];
            int n = 0;
            while (true) {
                File f = root.openNextFile();
                if (!f) break;
                if (!f.isDirectory()) {
                    String name = String(f.name());
                    if (name.startsWith("photo_") && name.endsWith(".jpg") && n < 64) {
                        bool isSmall = name.endsWith("s.jpg") && !name.endsWith("ss.jpg");
                        if (!isSmall) {
                            names[n] = name;
                            sizes[n] = (long)f.size();
                            ++n;
                        }
                    }
                }
                f.close();
            }
            root.close();
            for (int i = n - 1; i >= 0; --i) {
                const String& name = names[i];
                /* 缩略图用 photo_Ns.jpg 小图（~15KB，秒开）；没有则回退大图 */
                String small = name;
                small.replace(".jpg", "s.jpg");
                String kb = String((sizes[i] + 1023) / 1024) + "KB";
                html += "<div class='card'>"
                     "<div class='ph' onclick=\"lbOpen('" + name + "')\">"
                     "<img loading='lazy' src='/view?name=" + small +
                     "' onerror=\"this.onerror=null;this.src='/view?name=" + name + "'\">"
                     "<span class='badge'>" + kb + "</span></div>";
                html += "<div class='info'><span class='name'>" + name + "</span><span>";
                html += "<a onclick=\"lbOpen('" + name + "')\">看</a>";
                html += "<a href='/download?name=" + name + "' download>存</a>";
                html += "<a href='/delete?name=" + name
                     + "' onclick=\"return confirm('删除 " + name + "?')\">删</a>";
                html += "</span></div></div>";
                ++count;
            }
        }
    }
    if (count == 0) {
        html += "<div class='empty'><div class='big'>📷</div>"
             "暂无照片<br>去控制台拍一张，或按设备上的拍照键</div>";
    } else {
        html += "<script>document.getElementById('cnt').textContent=' 共 "
             + String(count) + " 张'</script>";
    }
    /* 灯箱：点卡片全屏看大图（下载/删除按钮跟随当前照片） */
    html += "</div>"
         "<div id='lb' onclick=\"if(event.target===this)lbClose()\">"
         "<img id='lbimg' alt=''>"
         "<div class='ops'><a class='btn' id='lbdl' download>⬇ 下载</a>"
         "<a class='btn rd' id='lbde'>🗑 删除</a>"
         "<a class='btn' onclick='lbClose()'>✕ 关闭</a></div></div>"
         "<script>"
         "function lbOpen(n){var i=document.getElementById('lbimg');"
         " i.src='/view?name='+n;i.dataset.n=n;"
         " document.getElementById('lbdl').href='/download?name='+n;"
         " document.getElementById('lbde').onclick=function(){"
         "  if(confirm('删除 '+n+' ?'))location.href='/delete?name='+n};"
         " document.getElementById('lb').style.display='block'}"
         "function lbClose(){var l=document.getElementById('lb');"
         " l.style.display='none';document.getElementById('lbimg').src=''}"
         "document.addEventListener('keydown',function(e){"
         " if(e.key==='Escape')lbClose()});"
         "</script>"
         "<div class='foot'>ESP32-S3 单板相机 · 照片存于 SD 卡 /photo_N.jpg</div>"
         "</div></body></html>";
    server.send(200, "text/html", html);
}

static void handleView() {
    if (!g_sdReady || !server.hasArg("name")) {
        server.send(503, "text/plain", "sd not ready or no name");
        return;
    }
    String name = server.arg("name");
    /* 防路径穿越 */
    if (name.indexOf('/') >= 0 || name.indexOf("..") >= 0) {
        server.send(400, "text/plain", "bad name");
        return;
    }
    String path = "/" + name;
    File f = SD.open(path, FILE_READ);
    if (!f) {
        server.send(404, "text/plain", "not found");
        return;
    }
    server.sendHeader("Cache-Control", "no-store");
    server.streamFile(f, "image/jpeg");
    f.close();
}

static void handleDownload() {
    if (!g_sdReady || !server.hasArg("name")) {
        server.send(503, "text/plain", "sd not ready or no name");
        return;
    }
    String name = server.arg("name");
    if (name.indexOf('/') >= 0 || name.indexOf("..") >= 0) {
        server.send(400, "text/plain", "bad name");
        return;
    }
    String path = "/" + name;
    File f = SD.open(path, FILE_READ);
    if (!f) {
        server.send(404, "text/plain", "not found");
        return;
    }
    server.sendHeader("Content-Disposition", "attachment; filename=" + name);
    server.streamFile(f, "application/octet-stream");
    f.close();
}

static void handleDelete() {
    if (!g_sdReady || !server.hasArg("name")) {
        server.send(503, "text/plain", "sd not ready or no name");
        return;
    }
    String name = server.arg("name");
    if (name.indexOf('/') >= 0 || name.indexOf("..") >= 0) {
        server.send(400, "text/plain", "bad name");
        return;
    }
    sd_app_delete_photo(name);
    server.sendHeader("Location", "/photos");
    server.send(302, "text/plain", "redirect");
}

/* 清空相册：删除 SD 卡根目录所有 photo_*.jpg（含小图），带 JS 二次确认 */
static void handleClearAll() {
    if (!g_sdReady) {
        server.send(503, "text/plain", "sd not ready");
        return;
    }
    int removed = 0;
    File root = SD.open("/");
    if (root) {
        File f = root.openNextFile();
        while (f) {
            if (!f.isDirectory()) {
                String name = String(f.name());
                if (name.startsWith("photo_") && name.endsWith(".jpg")) {
                    f.close();
                    SD.remove("/" + name);
                    ++removed;
                    f = root.openNextFile();
                    continue;
                }
            }
            f.close();
            f = root.openNextFile();
        }
        root.close();
    }
    lcd_app_mark_gallery_dirty();          /* 设备端相册列表同步过期 */
    Serial.printf("[WEB] 清空相册：删除 %d 张\n", removed);
    server.sendHeader("Location", "/photos");
    server.send(302, "text/plain", "redirect");
}

/* --------------------- 备忘录：/notes.txt 每行一条 --------------------- */
/* [首次写入修复] FILE_APPEND 打开不存在的文件时，ESP32 SD/vfs 不会创建，
 * 直接报 "does not exist, no permits for creation" —— 设备第一次用备忘
 * （或删空后）Web 添加必定失败。这里先显式建空文件；注意不能用 FILE_WRITE
 * 代替追加（它是截断模式，会把已有备忘清空） */
static void noteEnsureFile(void)
{
    if (SD.exists("/notes.txt")) return;
    File f = SD.open("/notes.txt", FILE_WRITE);
    if (f) f.close();
    Serial.println("[WEB] /notes.txt 不存在，已创建空文件");
}

static void handleNotes() {
    String html = "<!DOCTYPE html><html lang='zh'><head><meta charset='utf-8'>"
                  "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                  "<title>备忘录</title><style>"
                  "*{box-sizing:border-box}"
                  ":root{--txt:#e8edf4;--sub:#94a3b8;--line:rgba(148,163,184,.16);--card:rgba(17,24,36,.72)}"
                  "body{font-family:'Segoe UI',system-ui,sans-serif;color:var(--txt);padding:20px 16px 48px;margin:0;"
                  "background:#0a0e14;"
                  "background-image:radial-gradient(640px 340px at 12% -8%,rgba(56,189,248,.16),transparent 62%),"
                  "radial-gradient(520px 320px at 92% -4%,rgba(129,140,248,.14),transparent 62%)}"
                  ".wrap{max-width:640px;margin:0 auto}"
                  "h1{font-size:1.4em;display:flex;align-items:center;gap:12px;margin:6px 0 4px}"
                  "h1 .logo{width:34px;height:34px;border-radius:10px;display:grid;place-items:center;font-size:20px;"
                  "background:linear-gradient(135deg,#f59e0b,#fbbf24);box-shadow:0 4px 14px rgba(245,158,11,.3)}"
                  ".cnt{color:var(--sub);font-size:.5em;font-weight:400;margin-left:4px}"
                  ".panel{background:var(--card);border:1px solid var(--line);border-radius:16px;padding:14px;"
                  "backdrop-filter:blur(8px);box-shadow:0 8px 30px rgba(2,6,16,.45);margin:16px 0}"
                  "textarea{width:100%;height:72px;background:rgba(2,6,16,.5);color:var(--txt);"
                  "border:1px solid var(--line);border-radius:10px;padding:10px;font-size:.92em;resize:vertical;outline:none}"
                  "textarea:focus{border-color:rgba(56,189,248,.5)}"
                  "button,a.btn{background:rgba(148,163,184,.14);color:var(--txt);border:1px solid var(--line);"
                  "border-radius:10px;padding:10px 16px;cursor:pointer;text-decoration:none;font-size:.9em;"
                  "transition:.18s;font-weight:600;display:inline-block}"
                  "button.pri{background:linear-gradient(135deg,#1f6feb,#38bdf8);border:0;color:#fff;"
                  "box-shadow:0 4px 14px rgba(56,189,248,.3)}"
                  "button:hover,a.btn:hover{transform:translateY(-1px);filter:brightness(1.12)}"
                  "button.danger{background:rgba(239,68,68,.16);border-color:rgba(239,68,68,.35);color:#fca5a5}"
                  ".top{display:flex;gap:9px;margin-bottom:16px}"
                  ".note{background:rgba(2,6,16,.4);border:1px solid var(--line);border-radius:12px;"
                  "padding:10px 14px;margin:9px 0;display:flex;justify-content:space-between;gap:10px;align-items:center}"
                  ".note .no{color:var(--sub);font-family:Consolas,monospace;font-size:.8em;flex-shrink:0;"
                  "background:rgba(56,189,248,.12);border-radius:8px;padding:2px 8px}"
                  ".note .txt{flex:1;line-height:1.5;word-break:break-all}"
                  ".note a{color:#fca5a5;text-decoration:none;font-size:.85em;flex-shrink:0}"
                  ".note a:hover{text-decoration:underline}"
                  ".empty{color:var(--sub);text-align:center;padding:30px 10px;font-size:.92em}"
                  "</style></head><body><div class='wrap'>"
                  "<h1><span class='logo'>📝</span>备忘录<span class='cnt' id='cnt'></span></h1>"
                  "<div class='top'><a class='btn' href='/'>← 控制台</a></div>"
                  "<div class='panel'>"
                  "<textarea id='t' placeholder='输入备忘内容...'></textarea>"
                  "<div style='margin-top:10px'><button class='pri' "
                  "onclick=\"var v=document.getElementById('t').value.trim();"
                  "if(v)location.href='/noteadd?text='+encodeURIComponent(v)\">+ 添加</button></div></div>";
    noteEnsureFile();                    /* 首次打开也把空文件建好，读不再报错 */
    File f = SD.open("/notes.txt", FILE_READ);
    int i = 0;
    if (f) {
        while (f.available()) {
            String l = f.readStringUntil('\n');
            l.trim();
            if (!l.length()) continue;
            l.replace("<", "&lt;");
            l.replace(">", "&gt;");
            html += "<div class='note'><span class='no'>" + String(i + 1) + "</span>"
                    "<span class='txt'>" + l + "</span>"
                    "<a href='/notedel?i=" + String(i) + "'"
                    " onclick=\"return confirm('删除这条备忘？')\">删</a></div>";
            i++;
        }
        f.close();
    }
    if (i == 0) html += "<div class='empty'>暂无备忘 · 在上方输入并点添加</div>";
    else html += "<script>document.getElementById('cnt').textContent=' 共 "
              + String(i) + " 条'</script>";
    html += "</div></body></html>";
    server.send(200, "text/html", html);
}

static void handleNoteAdd() {
    String t = server.arg("text");
    t.trim();
    if (t.length()) {
        t.replace("\r", "");
        t.replace("\n", " ");
        noteEnsureFile();                /* 关键：先建文件再追加，否则首次添加静默失败 */
        File f = SD.open("/notes.txt", FILE_APPEND);
        if (f) { f.println(t); f.close(); }
    }
    server.sendHeader("Location", "/notes");
    server.send(303);
}

static void handleNoteDel() {
    int del = server.arg("i").toInt();
    String keep;
    File f = SD.open("/notes.txt", FILE_READ);
    if (f) {
        int i = 0;
        while (f.available()) {
            String l = f.readStringUntil('\n');
            l.trim();
            if (!l.length()) continue;
            if (i++ != del) keep += l + "\n";
        }
        f.close();
    }
    SD.remove("/notes.txt");
    File w = SD.open("/notes.txt", FILE_WRITE);
    if (w) { w.print(keep); w.close(); }
    server.sendHeader("Location", "/notes");
    server.send(303);
}

static void handleNotFound() {
    server.send(404, "text/plain", "404 Not Found");
}

/* WiFi 热点开关（TOP 长按 / 设置 App WiFi 界面）：
 * [需求变更] 开机不再自动开 AP（省电+防热），热点按需开启 */
static bool s_wifiOn = false;
static bool s_wifiConnecting = false;   /* WiFi 正在连接（照片流程避让用） */
bool web_app_wifi_busy(void) { return s_wifiConnecting; }
void web_app_toggle_wifi(void)
{
    s_wifiOn = !s_wifiOn;
    g_wifiState = s_wifiOn ? 1 : 0;
    if (!s_wifiOn) {
        WiFi.softAPdisconnect(true);
        WiFi.disconnect(true, true);      /* STA 也一并关（WiFi 总开关语义） */
        WiFi.mode(WIFI_OFF);
        Serial.println("[WEB] WiFi 已关闭（省电模式）");
    } else {
        bool staActive = (WiFi.status() == WL_CONNECTED);
        WiFi.mode(staActive ? WIFI_AP_STA : WIFI_AP);
        WiFi.softAP("ESP32-CAM", "12345678");
        Serial.println("[WEB] 热点模式已开启，IP=" + WiFi.softAPIP().toString()
                       + (staActive ? "（STA 保留）" : ""));
        web_app_ensure_server();
    }
}

/* [主界面 WiFi 开关] 像手机一样的总开关：开 = STA 模式自动连记忆热点
 * （/wifi.txt 轮流连接），关 = WiFi 全关省电。后台任务执行连接（不卡按键）。
 * 替代原 AP 开关语义：桌面/相机 App TOP 长按触发 */
static void wifiUserToggleTask(void* pv)
{
    Serial.println("[WEB] WiFi 开：STA 自动连接记忆热点...");
    web_app_connect_sta();
    vTaskDelete(nullptr);
}

void web_app_wifi_user_toggle(void)
{
    if (WiFi.status() == WL_CONNECTED || s_wifiConnecting) {
        /* 已连/正在连 → 关闭 */
        s_wifiConnecting = false;
        WiFi.disconnect(true, true);
        WiFi.mode(WIFI_OFF);
        s_wifiOn      = false;
        g_wifiState   = 0;
        s_serverStarted = false;   /* 接口上线后懒重启 */
        Serial.println("[WEB] WiFi 开关 → 已关闭（省电）");
    } else {
        s_wifiOn    = true;
        g_wifiState = 1;
        s_serverStarted = false;
        BaseType_t ok = xTaskCreate(wifiUserToggleTask, "wifiUsr", 8192,
                                    nullptr, 1, nullptr);
        if (ok != pdPASS) Serial.println("[WEB] 开关任务创建失败");
        else Serial.println("[WEB] WiFi 开关 → 开启（后台自动连）");
    }
}

/* =====================================================================
 * STA 单热点连接（WiFi 界面输密码连接 / 串口 wifista 复用）
 *   断连原因监听：reason=15 握手超时=密码错；2=认证超时
 * 成功打印设备 IP（同网 PC/手机浏览器直接访问该 IP 看照片）；
 * 失败返回 false（不自动回退 AP，由调用方决定后续）
 * =================================================================== */
bool web_app_connect_ssid(const String& ssid, const String& pass, uint32_t timeoutMs)
{
    s_wifiConnecting = true;      /* 照片流程避让：期间拍照自动降 UXGA */
    static bool s_evtReg = false;
    if (!s_evtReg) {
        WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
            Serial.printf("[WEB] STA 断开事件 reason=%d\n",
                          info.wifi_sta_disconnected.reason);
        }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
        s_evtReg = true;
    }
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    /* [修复] 连接前强制刷新驱动扫描缓存：直接 begin 走"快速连接"用旧缓存，
     * 缓存里没有目标时秒回 NO_SSID_AVAIL，不再做全扫 → 关联失败。
     * [注意] 扫描结果必须保留到 begin 之后（scanDelete 会清掉驱动缓存，
     * 导致快速连接找不到目标卡 IDLE） */
    WiFi.setSleep(false);              /* 关省电：modem sleep 会致 beacon 丢失关联失败 */
    int found = -1;
    int nScan = WiFi.scanNetworks();
    for (int i = 0; i < nScan; ++i) {
        if (WiFi.SSID(i) == ssid) {
            found = i;
            break;
        }
    }
    if (found >= 0) {
        Serial.printf("[WEB] 扫描确认目标可见: %s ch%d %ddBm enc=%d\n",
                      ssid.c_str(), WiFi.channel(found), WiFi.RSSI(found),
                      WiFi.encryptionType(found));
    } else {
        Serial.printf("[WEB] 扫描 %d 个网络，未见目标 %s（热点已关/休眠？）\n",
                      nScan, ssid.c_str());
    }
    /* [修复] IDLE 卡死根因 = 提前 scanDelete 清了驱动缓存（快速连接找不到
     * 目标就永远停在 IDLE）。扫描缓存保留到连接结束后再删。
     * [修复2] Windows 移动热点节电模式常把首个认证晾超时（reason=2
     * AUTH_EXPIRE），驱动不会自动重试 → 这里手动重试 3 次 */
    bool ok = false;
    int st;
    for (int attempt = 1; attempt <= 3 && !ok; ++attempt) {
        if (found >= 0) {
            st = WiFi.begin(ssid.c_str(), pass.c_str(),
                            WiFi.channel(found), WiFi.BSSID(found));
            Serial.printf("[WEB] 尝试 %d/3（信道+BSSID 直连）begin=%d\n",
                          attempt, st);
        } else {
            st = WiFi.begin(ssid.c_str(), pass.c_str());
            Serial.printf("[WEB] 尝试 %d/3 begin=%d\n", attempt, st);
        }
        uint32_t t1 = millis();
        int lastSt = -1;
        while (millis() - t1 < 10000) {
            int s = WiFi.status();
            if (s != lastSt) {   /* 状态变化打印：定位卡在关联还是 DHCP */
                const char* names[] = {"IDLE", "NO_SSID_AVAIL", "SCAN_COMPLETED",
                                       "CONNECTED", "CONNECT_FAILED", "CONNECTION_LOST",
                                       "DISCONNECTED"};
                Serial.printf("[WEB] %2us status=%s\n",
                              (unsigned)((millis() - t1) / 1000),
                              (s >= 0 && s <= 6) ? names[s] : "?");
                lastSt = s;
            }
            if (s == WL_CONNECTED) { ok = true; break; }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (!ok) {
            WiFi.disconnect(false, true);
            vTaskDelay(pdMS_TO_TICKS(400));
        }
    }
    if (ok) {
        Serial.printf("[WEB] STA 已连接 %s，设备 IP = %s（浏览器直接访问）\n",
                      ssid.c_str(), WiFi.localIP().toString().c_str());
        g_wifiState = 1;
        s_wifiOn = true;       /* [修复] 与热点开关状态同步，否则 toggle 会误判 */
        web_app_ensure_server();
        WiFi.scanDelete();     /* 连接完成后释放扫描缓存 */
        s_wifiConnecting = false;
        /* [桌面时钟] 联网即后台 NTP 授时（北京时间，失败不影响其它功能） */
        configTime(8 * 3600, 0, "ntp.aliyun.com", "ntp1.aliyun.com", "pool.ntp.org");
        return true;
    }
    Serial.printf("[WEB] 3 次尝试均失败（最终 status=%d）\n", WiFi.status());
    WiFi.disconnect(false, true);
    WiFi.scanDelete();
    g_wifiState = 0;
    s_wifiConnecting = false;
    return false;
}

/* 当前 WiFi 状态简述（设置页 WiFi 行） */
String web_app_wifi_status_text(void)
{
    wifi_mode_t m = WiFi.getMode();
    if ((m & WIFI_MODE_STA) && WiFi.status() == WL_CONNECTED)
        return WiFi.localIP().toString();
    if (m & WIFI_MODE_AP)
        return "AP " + WiFi.softAPIP().toString();
    if (m & WIFI_MODE_STA)
        return "STA...";
    return "OFF";
}

/* =====================================================================
 * STA 固定热点轮流连接（串口 wifista 命令触发）
 *   热点列表存 SD 卡 /wifi.txt，每行一条：SSID 密码（空格分隔）
 * 依次尝试连接，成功即停并打印设备 IP；全部失败自动回退 AP 模式
 * =================================================================== */
bool web_app_connect_sta(void)
{
    /* 1. 读热点列表 */
    File f = SD.open("/wifi.txt", FILE_READ);
    if (!f) {
        Serial.println("[WEB] /wifi.txt 不存在（用 wifiadd SSID 密码 添加）");
        return false;
    }
    String lines[8];
    int n = 0;
    while (f.available() && n < 8) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length()) lines[n++] = line;
    }
    f.close();
    if (n == 0) {
        Serial.println("[WEB] /wifi.txt 是空的（用 wifiadd SSID 密码 添加）");
        return false;
    }

    /* 2. 轮流尝试 */
    for (int i = 0; i < n; ++i) {
        int sp = lines[i].indexOf(' ');
        if (sp <= 0) continue;
        String ssid = lines[i].substring(0, sp);
        String pass = lines[i].substring(sp + 1);
        pass.trim();
        if (web_app_connect_ssid(ssid, pass, 15000)) return true;
        Serial.println("[WEB] 连接失败，试下一个");
    }

    /* 3. 全部失败 → 回退 AP */
    WiFi.mode(WIFI_AP);
    WiFi.softAP("ESP32-CAM", "12345678");
    s_wifiOn = true;
    Serial.println("[WEB] 所有热点均失败，已回退 AP 192.168.4.1");
    g_wifiState = 1;
    web_app_ensure_server();
    return false;
}

/* =====================================================================
 * 开机自动连接任务（后台低优先级，不阻塞开机流程）
 *   SD 卡 /wifi.txt 有记忆的热点（屏幕键盘输过密码的都会记住）→
 *   延时 4s 等驱动/SD 就绪 → 轮流连接；全部失败回退 AP 192.168.4.1
 * =================================================================== */
static void wifiAutoTask(void* pv)
{
    vTaskDelay(pdMS_TO_TICKS(4000));
    File f = SD.open("/wifi.txt", FILE_READ);
    if (!f) { vTaskDelete(nullptr); return; }
    bool hasEntry = f.available();
    f.close();
    if (!hasEntry) { vTaskDelete(nullptr); return; }   /* 无记忆：保持 WiFi 关闭 */
    Serial.println("[WEB] 自动连接记忆热点（/wifi.txt）...");
    web_app_connect_sta();
    vTaskDelete(nullptr);
}

void web_app_init(void)
{
    /* [需求变更] 开机不再自动开 AP：WiFi 保持关闭（省电 + 防热）。
     * 需要时：设置 App → WiFi 管理 → 热点模式 / 扫描连接；
     * 或任意界面 TOP 长按切换热点。WebServer 先起，接口上线后即可访问 */
    WiFi.mode(WIFI_OFF);
    g_wifiState = 0;
    Serial.println("[WEB] WiFi 默认关闭（设置 App → WiFi 管理 按需开启）");

    /* 注册路由 */
    server.on("/",             HTTP_GET, handleRoot);
    server.on("/preview",      HTTP_GET, handlePreview);
    server.on("/snap",         HTTP_GET, handleSnap);
    server.on("/status",       HTTP_GET, handleStatus);
    server.on("/capture",      HTTP_GET, handleCapture);
    server.on("/photos",       HTTP_GET, handlePhotos);
    server.on("/view",         HTTP_GET, handleView);
    server.on("/download",     HTTP_GET, handleDownload);
    server.on("/delete",       HTTP_GET, handleDelete);
    server.on("/clearall",     HTTP_GET, handleClearAll);
    server.on("/notes",        HTTP_GET, handleNotes);
    server.on("/noteadd",      HTTP_GET, handleNoteAdd);
    server.on("/notedel",      HTTP_GET, handleNoteDel);
    server.onNotFound(handleNotFound);

    /* WiFi 关闭时不 begin（lwIP 未启动会崩溃），接口上线后懒启动 */
    Serial.println("[WEB] 路由注册完成（WiFi 接口上线后 WebServer 自动启动）");

    /* [自动连] 有记忆热点时开机后台自动连接（不阻塞开机流程） */
    xTaskCreate(wifiAutoTask, "wifiAuto", 8192, nullptr, 1, nullptr);

    /* [取景流] PSRAM 双缓冲 + 抓帧任务（网页请求驱动，设备拍照时让路） */
    s_streamBuf[0] = (uint8_t*)heap_caps_malloc(STREAM_BUF_SIZE, MALLOC_CAP_SPIRAM);
    s_streamBuf[1] = (uint8_t*)heap_caps_malloc(STREAM_BUF_SIZE, MALLOC_CAP_SPIRAM);
    s_streamMutex  = xSemaphoreCreateMutex();
    if (s_streamBuf[0] && s_streamBuf[1] && s_streamMutex) {
        xTaskCreate(streamTask, "stream", 4096, nullptr, 1, nullptr);
        Serial.println("[WEB] 取景流任务就绪（VGA 双缓冲 2x96KB）");
    } else {
        Serial.println("[WEB] 取景流缓冲分配失败（网页取景/拍照不可用）");
    }
}

void web_app_handle(void)
{
    server.handleClient();
}
