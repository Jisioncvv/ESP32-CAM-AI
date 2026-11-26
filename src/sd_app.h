/*
 * =====================================================================
 * sd_app.h  ——  TF 卡模块接口
 * =====================================================================
 */
#pragma once
#include <Arduino.h>

/* 初始化 SD 卡（HSPI 总线） */
bool sd_app_init(void);

/* 是否已就绪 */
bool sd_app_is_ready(void);

/* 计算下一张照片的存储路径：遍历找出最大编号 +1 */
String sd_app_get_next_photo_path(void);

/* 保存一帧 JPEG 到 SD 卡 */
bool sd_app_save_photo(const String& path, const uint8_t* data, size_t len);

/* 删除指定照片 */
bool sd_app_delete_photo(const String& name);

/* 按编号删除照片（大图 photo_N.jpg + 小图 photo_Ns.jpg），供串口 del 命令 */
bool sd_app_delete_number(int n);

/* 清空相册：删除根目录全部 photo_*.jpg（含小图），返回删除数量 */
int sd_app_clear_all(void);

/* 串口回传照片（调试，getphoto 命令）：idx=1 最新，2 次新…
 * small=true 回传同名小图 photo_Ns.jpg（诊断缩略图问题用）
 * 协议：PHOTO <name> <size>\n + 多行 Base64 + ENDPHOTO\n */
bool sd_app_dump_photo(int idx, bool small = false);
