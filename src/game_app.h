/*
 * =====================================================================
 * game_app.h  ——  游戏 App（多游戏选择）
 * =================================================================== */
#pragma once

#include <Arduino.h>

void game_app_loop(void);

/* 输入（keyTask 累加计数，游戏循环逐个消费）：
 *   g_gameUp/gameDown = TOP/DOWN 单击（含长按连发）
 *   g_gameCam         = CAM 单击
 *   g_gameCamHold     = CAM 长按（俄罗斯方块硬降）
 *   g_gameMidReq      = MID 单击（菜单=退出 App，游戏内=回菜单） */
extern volatile int  g_gameUp;
extern volatile int  g_gameDown;
extern volatile int  g_gameCam;
extern volatile int  g_gameCamHold;
extern volatile bool g_gameMidReq;
