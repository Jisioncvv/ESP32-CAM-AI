/*
 * =====================================================================
 * sd_app.cpp  ——  TF 卡（SD）模块实现
 * ---------------------------------------------------------------------
 * 使用独立 HSPI 总线：SCK=13 / MISO=14 / MOSI=12 / CS=11
 * 照片命名规则：photo_N.jpg，N 为最大编号 + 1
 * =====================================================================
 */
#include "config.h"
#include "sd_app.h"
#include "lcd_app.h"
#include <SPI.h>
#include <SD.h>
#include <mbedtls/base64.h>

bool g_sdReady = false;

static SPIClass g_spi(HSPI);   /* 独立 HSPI 总线 */

bool sd_app_init(void)
{
    Serial.println("[SD] 初始化中...");
    g_spi.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);

    /* 重试 3 次：先用 1MHz 低频（供电/接触边缘时 CMD0 更容易成功），逐次提速。
     * cmd 0x00 失败 = 卡连 GO_IDLE 都不应答，多为供电/接线/初始时钟问题。
     * [提速] 低频挂载成功后继续逐级提速，用能稳定工作的最高频率
     * （实测接触边缘的卡 1MHz 能挂上但后续大文件读写会失败） */
    const uint32_t freqs[3] = {1000000U, 4000000U, 10000000U};
    int best = -1;
    for (int i = 0; i < 3; ++i) {
        SD.end();   /* 清理上一次挂载状态后重试 */
        if (SD.begin(PIN_SD_CS, g_spi, freqs[i], "/sd", 5, false)) {
            g_sdReady = true;
            best = i;
            Serial.printf("[SD] %lu MHz 挂载成功\n", freqs[i] / 1000000U);
            if (i < 2) vTaskDelay(pdMS_TO_TICKS(100));   /* 提速前缓冲 */
        } else {
            break;   /* 本级失败：用上一级成功频率（保持挂载状态） */
        }
    }
    if (best >= 0) {
        if (best < 2) {   /* 最后一级失败过：退回上一成功频率重新挂载 */
            SD.end();
            SD.begin(PIN_SD_CS, g_spi, freqs[best], "/sd", 5, false);
        }
        Serial.printf("[SD] 初始化成功（工作频率 %lu MHz）\n", freqs[best] / 1000000U);
        return true;
    }
    Serial.println("[SD] 初始化失败（检查卡插好/供电/接线 SCK13 MISO14 MOSI12 CS11）");
    g_sdReady = false;
    return false;
}

bool sd_app_is_ready(void)
{
    return g_sdReady;
}

String sd_app_get_next_photo_path(void)
{
    int maxIdx = 0;
    File root = SD.open("/");
    if (!root) return "/photo_1.jpg";

    while (true) {
        File f = root.openNextFile();
        if (!f) break;
        String name = String(f.name());
        if (!f.isDirectory() && name.startsWith("photo_") && name.endsWith(".jpg")) {
            /* 提取 N */
            int s = name.indexOf('_');
            int e = name.indexOf('.');
            if (s > 0 && e > s) {
                int n = name.substring(s + 1, e).toInt();
                if (n > maxIdx) maxIdx = n;
            }
        }
        f.close();
    }
    root.close();
    String path = "/" + String("photo_") + String(maxIdx + 1) + String(".jpg");
    Serial.printf("[SD] 下一张照片路径: %s\n", path.c_str());
    return path;
}

bool sd_app_save_photo(const String& path, const uint8_t* data, size_t len)
{
    if (!g_sdReady) return false;
    /* [残图修复] SD 偶发瞬时 SPI 错误会让单次 write 少写几字节（实测
     * photo_15s 只写进 16384/16403，缺 JPEG 结尾 EOI → 相册缩略图解码
     * 出马赛克/蓝噪）。写完必须回读校验长度，不符就整文件重写 */
    for (int attempt = 0; attempt < 2; ++attempt) {
        File f = SD.open(path, FILE_WRITE);
        if (!f) {
            Serial.println("[SD] 打开写入失败");
            return false;
        }
        /* 分块写入 + 短写重试 */
        size_t off = 0;
        for (int retry = 0; off < len && retry < 4; ++retry) {
            size_t w = f.write(data + off, len - off);
            if (w == 0) vTaskDelay(pdMS_TO_TICKS(20));   /* 连续零写入：稍等再试 */
            off += w;
        }
        f.close();

        size_t sz = 0;
        File chk = SD.open(path, FILE_READ);
        if (chk) { sz = chk.size(); chk.close(); }
        if (sz == len) {
            Serial.printf("[SD] 照片已保存: %s (%u 字节)\n", path.c_str(), (unsigned)len);
            return true;
        }
        Serial.printf("[SD] 写入长度不符 %u / %u，重写（第 %d 次）\n",
                      (unsigned)sz, (unsigned)len, attempt + 1);
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    return false;
}

bool sd_app_delete_photo(const String& name)
{
    String path = "/" + name;
    bool ok = SD.remove(path);
    if (ok) Serial.printf("[SD] 已删除: %s\n", path.c_str());
    else    Serial.printf("[SD] 删除失败: %s\n", path.c_str());
    return ok;
}

/* 按编号删除：photo_N.jpg + photo_Ns.jpg，删完标记相册列表过期 */
bool sd_app_delete_number(int n)
{
    if (!g_sdReady || n <= 0) return false;
    String big   = "/photo_" + String(n) + ".jpg";
    String small = "/photo_" + String(n) + "s.jpg";
    bool ok = false;
    if (SD.exists(big))  ok = SD.remove(big);
    if (SD.exists(small)) SD.remove(small);
    Serial.printf("[SD] 删除 photo_%d%s（大图%s）\n", n,
                  SD.exists(small) ? " 小图残留" : "", ok ? " OK" : " 不存在/失败");
    if (ok) lcd_app_mark_gallery_dirty();
    return ok;
}

/* 清空相册：删除根目录全部 photo_*.jpg（含配套小图 photo_Ns.jpg） */
int sd_app_clear_all(void)
{
    int removed = 0;
    File root = SD.open("/");
    if (!root) {
        Serial.println("[SD] 清空失败：根目录打开失败");
        return 0;
    }
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
    Serial.printf("[SD] 清空相册完成：删除 %d 张\n", removed);
    return removed;
}

/* =====================================================================
 * 串口回传照片（getphoto 命令，配合 tools/fetch_photos.py）：
 *   收集大图 photo_<数字>.jpg（跳过小图 photo_Ns.jpg），按编号降序，
 *   idx=1 最新。分块读卡 → Base64 分行输出（USB-CDC 全速，几百 KB 秒传）
 * =================================================================== */
bool sd_app_dump_photo(int idx, bool small)
{
    /* 1. 收集编号（大图 photo_N.jpg / 小图 photo_Ns.jpg），按编号降序，idx=1 最新
     * [容量 bug 修复] 原来 nums[64] 且到 64 就停收 —— 目录按创建顺序遍历，
     * 于是"最早 64 张"占满名额，之后拍的照片 getphoto 永远取不到（相册同理）。
     * 现在容量 200，满员时挤掉编号最小的，保证取的永远是"最新的几张" */
    #define SD_DUMP_MAX 200
    int nums[SD_DUMP_MAX];
    int n = 0;
    File root = SD.open("/");
    if (root) {
        File f = root.openNextFile();
        while (f) {
            if (!f.isDirectory()) {
                String name = String(f.name());
                if (name.startsWith("photo_") && name.endsWith(".jpg")) {
                    String num = name.substring(6, name.length() - 4);
                    if (small) {
                        /* 小图：photo_Ns.jpg → num 需以 's' 结尾 */
                        if (!num.endsWith("s")) { f.close(); f = root.openNextFile(); continue; }
                        num = num.substring(0, num.length() - 1);
                    } else if (num.endsWith("s")) {
                        f.close(); f = root.openNextFile(); continue;
                    }
                    bool digits = num.length() > 0;
                    for (unsigned int i = 0; i < num.length() && digits; ++i)
                        if (!isDigit(num.charAt(i))) digits = false;
                    if (digits) {
                        int v = num.toInt();
                        int pos = n;
                        while (pos > 0 && nums[pos - 1] > v) --pos;
                        if (n >= SD_DUMP_MAX) {
                            if (pos == 0) {                    /* 更旧 → 丢 */
                                f.close(); f = root.openNextFile(); continue;
                            }
                            for (int i = 0; i < SD_DUMP_MAX - 1; ++i) nums[i] = nums[i + 1];
                            n = SD_DUMP_MAX - 1;
                            --pos;
                        }
                        for (int i = n; i > pos; --i) nums[i] = nums[i - 1];
                        nums[pos] = v;
                        ++n;
                    }
                }
            }
            f.close();
            f = root.openNextFile();
        }
        root.close();
    }
    for (int a = 0; a < n; ++a)                       /* 编号降序（最新在前） */
        for (int b = a + 1; b < n; ++b)
            if (nums[b] > nums[a]) { int t = nums[a]; nums[a] = nums[b]; nums[b] = t; }

    if (idx > n) {
        Serial.printf("[SD] dump 失败：匹配大图 %d 张（SD 就绪=%d）\n", n, (int)g_sdReady);
        File d = SD.open("/");
        if (d) {
            File df = d.openNextFile();
            int shown = 0;
            while (df && shown < 16) {
                Serial.printf("[SD] 根目录: %s%s\n", df.name(), df.isDirectory() ? "/" : "");
                df.close();
                df = d.openNextFile();
                ++shown;
            }
            d.close();
        } else {
            Serial.println("[SD] 根目录打开失败（未挂载？）");
        }
        Serial.println("PHOTO NONE 0");
        Serial.println("ENDPHOTO");
        return false;
    }
    String path = "/photo_" + String(nums[idx - 1]) + (small ? "s.jpg" : ".jpg");
    File f = SD.open(path, FILE_READ);
    if (!f) {
        Serial.println("PHOTO NONE 0");
        Serial.println("ENDPHOTO");
        return false;
    }

    size_t size = f.size();
    Serial.printf("PHOTO %s %u\n", path.c_str(), (unsigned)size);

    /* 2. 1024B 一块 → 1368 字符 Base64 行（static 缓冲避免栈压力）
     * 读失败短重试：SD 接触边缘时偶发单次读 0 字节 */
    static uint8_t rbuf[1024];
    static char    bbuf[1400];
    size_t sent = 0;
    while (sent < size) {
        int r = -1;
        for (int retry = 0; r <= 0 && retry < 5; ++retry) {
            f.seek(sent);   /* [容错] 从断点重读：SD 接触边缘时偶发读 0 字节 */
            size_t want = (sent + sizeof(rbuf) > size) ? (size - sent) : sizeof(rbuf);
            r = f.read(rbuf, want);
            if (r <= 0) vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (r <= 0) {
            Serial.printf("[SD] dump 读失败@%u（pos=%u，break）\n",
                          (unsigned)sent, (unsigned)f.position());
            break;
        }
        size_t olen = 0;
        int enc = mbedtls_base64_encode((unsigned char*)bbuf, sizeof(bbuf), &olen,
                                        rbuf, (size_t)r);
        if (enc != 0 || olen == 0) {
            Serial.printf("[SD] dump b64编码失败 enc=%d olen=%u r=%d\n",
                          enc, (unsigned)olen, r);
            break;
        }
        bbuf[olen] = '\0';
        Serial.println(bbuf);
        sent += (size_t)r;
    }
    f.close();
    Serial.printf("[SD] dump 完成: %u / %u 字节\n", (unsigned)sent, (unsigned)size);
    Serial.println("ENDPHOTO");
    return true;
}
