/*
 * =====================================================================
 * game_app.cpp  ——  游戏 App：选择页 + 三款游戏
 * ---------------------------------------------------------------------
 *   1) Tetris  俄罗斯方块（TOP/DOWN 平移，CAM 旋转，CAM 长按硬降）
 *   2) Snake   贪吃蛇（TOP 左转，CAM 右转，DOWN 加速）
 *   3) 2048    数字合并（TOP/DOWN 换方向，CAM 执行）
 *
 * 视觉：全部走 theme.h 的配色与控件（标题栏 30px / 卡片圆角 10px），
 *       深色/浅色主题下都能看。
 * 最高分存在 SD /games.txt（一行一款：0=1240），SD 缺失时只在内存里。
 * 栈：所有棋盘/蛇身一律 static —— 局部大数组曾把 8KB 任务栈撑爆。
 * =================================================================== */
#include "config.h"
#include "game_app.h"
#include "theme.h"
#include "lcd_app.h"
#include <SD.h>

volatile int  g_gameUp = 0;
volatile int  g_gameDown = 0;
volatile int  g_gameCam = 0;
volatile int  g_gameCamHold = 0;
volatile bool g_gameMidReq = false;

#define GAME_COUNT 3
static const char* s_gameNames[GAME_COUNT] = { "Tetris", "Snake", "2048" };
static const char* s_gameNote[GAME_COUNT]  = { "clear lines", "eat apples", "merge tiles" };
static int  s_best[GAME_COUNT] = { 0, 0, 0 };

static Drawer mkdraw(void) { Drawer g = { lcd_app_get_spr(), &tft }; return g; }

/* 整帧推屏（Sprite 可用时）：一次写屏，无闪烁 */
static void pushFrame(Drawer& g) { if (g.s) g.s->pushSprite(0, 0); }

/* --------------------- 最高分：/games.txt --------------------- */
static void bestLoad(void)
{
    File f = SD.open("/games.txt", FILE_READ);
    if (!f) return;
    while (f.available()) {
        String l = f.readStringUntil('\n');
        int eq = l.indexOf('=');
        if (eq <= 0) continue;
        int idx = l.substring(0, eq).toInt();
        int val = l.substring(eq + 1).toInt();
        if (idx >= 0 && idx < GAME_COUNT && val > s_best[idx]) s_best[idx] = val;
    }
    f.close();
}

static void bestSave(void)
{
    if (!SD.exists("/games.txt")) {
        File nf = SD.open("/games.txt", FILE_WRITE);
        if (nf) nf.close();
    }
    SD.remove("/games.txt");
    File f = SD.open("/games.txt", FILE_WRITE);
    if (!f) return;
    for (int i = 0; i < GAME_COUNT; ++i) {
        f.print(String(i) + "=" + String(s_best[i]) + "\n");
    }
    f.close();
}

static void bestUpdate(int idx, int score)
{
    if (idx < 0 || idx >= GAME_COUNT || score <= s_best[idx]) return;
    s_best[idx] = score;
    bestSave();
    Serial.printf("[GAME] %s 新纪录 %d\n", s_gameNames[idx], score);
}

/* --------------------- 输入消费 --------------------- */
static void inputDrain(void)
{
    g_gameUp = g_gameDown = g_gameCam = g_gameCamHold = 0;
    g_gameMidReq = false;
}
static bool takeUp(void)     { if (g_gameUp > 0)     { --g_gameUp;     return true; } return false; }
static bool takeDown(void)   { if (g_gameDown > 0)   { --g_gameDown;   return true; } return false; }
static bool takeCam(void)    { if (g_gameCam > 0)    { --g_gameCam;    return true; } return false; }
static bool takeCamHold(void){ if (g_gameCamHold > 0){ --g_gameCamHold;return true; } return false; }

/* =====================================================================
 * 游戏选择页
 * =================================================================== */
static void menuDraw(Drawer& g, int sel)
{
    const Theme* th = g_th;
    g.fillScreen(th->bg);
    ui_header(g, "Games", "best scores");
    ui_footer(g, "TOP/DN select   CAM play   MID back");

    for (int i = 0; i < GAME_COUNT; ++i) {
        int y = 36 + i * 58;
        bool on = (i == sel);
        uint16_t rowBg = on ? th->selBg : th->surface;
        ui_row(g, 10, y, 300, 50, on);

        /* 图标块 */
        g.fillRoundRect(22, y + 8, 34, 34, 9, th->surface2);
        uint16_t ic = on ? th->accent : th->muted;
        if (i == 0) {                     /* Tetris：三个小方块 */
            g.fillRect(30, y + 14, 8, 8, ic);
            g.fillRect(40, y + 14, 8, 8, ic);
            g.fillRect(30, y + 24, 8, 8, ic);
        } else if (i == 1) {              /* Snake：一条折线 + 果子 */
            g.fillRect(28, y + 16, 18, 4, ic);
            g.fillRect(42, y + 16, 4, 12, ic);
            g.fillCircle(45, y + 32, 3, th->danger);
        } else {                          /* 2048：2x2 格子 */
            g.fillRect(29, y + 14, 9, 9, ic);
            g.fillRect(41, y + 14, 9, 9, ic);
            g.fillRect(29, y + 26, 9, 9, ic);
            g.fillRect(41, y + 26, 9, 9, th->surface2);
        }

        g.setTextDatum(TL_DATUM);
        g.setTextFont(2);
        g.setTextSize(1);
        g.setTextColor(th->text, rowBg);
        g.drawString(s_gameNames[i], 68, y + 10);

        g.setTextFont(1);
        g.setTextColor(th->muted, rowBg);
        g.drawString(String(s_gameNote[i]) + "   best " + String(s_best[i]), 68, y + 30);

        if (on) {
            g.setTextDatum(TR_DATUM);
            g.setTextColor(th->accent, rowBg);
            g.drawString("play", 300, y + 20);
            g.setTextDatum(TL_DATUM);
        }
    }
}

/* =====================================================================
 * 1) 俄罗斯方块
 * =================================================================== */
#define TET_COLS 10
#define TET_ROWS 20
#define TET_CELL 10

static const uint8_t s_tetShapes[7][4][8] = {
    { {0,1,1,1,2,1,3,1}, {2,0,2,1,2,2,2,3}, {0,2,1,2,2,2,3,2}, {1,0,1,1,1,2,1,3} },
    { {1,0,2,0,1,1,2,1}, {1,0,2,0,1,1,2,1}, {1,0,2,0,1,1,2,1}, {1,0,2,0,1,1,2,1} },
    { {1,0,0,1,1,1,2,1}, {1,0,1,1,2,1,1,2}, {0,1,1,1,2,1,1,2}, {1,0,0,1,1,1,1,2} },
    { {1,0,2,0,0,1,1,1}, {1,0,1,1,2,1,2,2}, {1,1,2,1,0,2,1,2}, {0,0,0,1,1,1,1,2} },
    { {0,0,1,0,1,1,2,1}, {2,0,1,1,2,1,1,2}, {0,1,1,1,1,2,2,2}, {1,0,0,1,1,1,0,2} },
    { {0,0,0,1,1,1,2,1}, {1,0,2,0,1,1,1,2}, {0,1,1,1,2,1,2,2}, {1,0,1,1,0,2,1,2} },
    { {2,0,0,1,1,1,2,1}, {1,0,1,1,1,2,2,2}, {0,1,1,1,2,1,0,2}, {0,0,1,0,1,1,1,2} },
};
static const uint16_t s_tetColors[7] = {
    0x07FF, 0xFFE0, 0xF81F, 0x07E0, 0xF800, 0x001F, 0xFD20
};
static uint8_t s_tetBoard[TET_ROWS][TET_COLS];

static bool tet_collide(int t, int r, int px, int py)
{
    const uint8_t* c = s_tetShapes[t][r];
    for (int i = 0; i < 4; ++i) {
        int bx = px + c[i * 2], by = py + c[i * 2 + 1];
        if (bx < 0 || bx >= TET_COLS || by >= TET_ROWS) return true;
        if (by >= 0 && s_tetBoard[by][bx]) return true;
    }
    return false;
}
static void tet_lock(int t, int r, int px, int py)
{
    const uint8_t* c = s_tetShapes[t][r];
    for (int i = 0; i < 4; ++i) {
        int bx = px + c[i * 2], by = py + c[i * 2 + 1];
        if (by >= 0 && by < TET_ROWS && bx >= 0 && bx < TET_COLS)
            s_tetBoard[by][bx] = (uint8_t)(t + 1);
    }
}
static int tet_clear_lines(void)
{
    int cleared = 0;
    for (int y = TET_ROWS - 1; y >= 0; --y) {
        bool full = true;
        for (int x = 0; x < TET_COLS; ++x)
            if (!s_tetBoard[y][x]) { full = false; break; }
        if (full) {
            ++cleared;
            for (int yy = y; yy > 0; --yy)
                for (int x = 0; x < TET_COLS; ++x) s_tetBoard[yy][x] = s_tetBoard[yy - 1][x];
            for (int x = 0; x < TET_COLS; ++x) s_tetBoard[0][x] = 0;
            ++y;
        }
    }
    return cleared;
}

static void tetDraw(Drawer& g, int curT, int curR, int curX, int curY,
                    int nextT, int score, int lines, bool over)
{
    const Theme* th = g_th;
    const int BX = 10, BY = 32;
    g.fillScreen(th->bg);
    ui_header(g, "Tetris", "MID back");

    /* 棋盘 */
    g.fillRoundRect(BX - 3, BY - 3, TET_COLS * TET_CELL + 6, TET_ROWS * TET_CELL + 6,
                    6, th->surface2);
    for (int y = 0; y < TET_ROWS; ++y)
        for (int x = 0; x < TET_COLS; ++x)
            if (s_tetBoard[y][x])
                g.fillRect(BX + x * TET_CELL + 1, BY + y * TET_CELL + 1,
                           TET_CELL - 2, TET_CELL - 2, s_tetColors[s_tetBoard[y][x] - 1]);
    if (!over) {
        const uint8_t* c = s_tetShapes[curT][curR];
        for (int i = 0; i < 4; ++i) {
            int bx = curX + c[i * 2], by = curY + c[i * 2 + 1];
            if (by >= 0)
                g.fillRect(BX + bx * TET_CELL + 1, BY + by * TET_CELL + 1,
                           TET_CELL - 2, TET_CELL - 2, s_tetColors[curT]);
        }
    }

    /* 右侧面板 */
    const int PX = 122, PW = 188;
    ui_card(g, PX, BY, PW, 200, th->surface);
    g.setTextDatum(MC_DATUM);
    g.setTextFont(1);
    g.setTextSize(1);
    g.setTextColor(th->muted, th->surface);
    g.drawString("NEXT", PX + 44, BY + 16);

    {   /* Next 预览：4x4 盒居中，格子 8px */
        const uint8_t* c = s_tetShapes[nextT][0];
        int mx = 3, Mx = 0, my = 3, My = 0;
        for (int i = 0; i < 4; ++i) {
            int cx = c[i * 2], cy = c[i * 2 + 1];
            if (cx < mx) mx = cx;
            if (cx > Mx) Mx = cx;
            if (cy < my) my = cy;
            if (cy > My) My = cy;
        }
        int ox = PX + 44 - ((Mx - mx + 1) * 8) / 2 - mx * 8;
        int oy = BY + 40 - ((My - my + 1) * 8) / 2 - my * 8;
        for (int i = 0; i < 4; ++i)
            g.fillRect(ox + c[i * 2] * 8, oy + c[i * 2 + 1] * 8, 7, 7, s_tetColors[nextT]);
    }

    g.setTextDatum(TL_DATUM);
    g.setTextColor(th->muted, th->surface);
    g.drawString("SCORE", PX + 16, BY + 84);
    g.drawString("LINES", PX + 16, BY + 118);
    g.setTextFont(2);
    g.setTextColor(th->text, th->surface);
    g.drawString(String(score), PX + 16, BY + 96);
    g.drawString(String(lines), PX + 16, BY + 130);

    g.setTextFont(1);
    g.setTextColor(th->muted, th->surface);
    g.drawString("TOP/DN  move", PX + 16, BY + 160);
    g.drawString("CAM     rotate", PX + 16, BY + 174);
    g.drawString("hold    drop", PX + 16, BY + 188);

    if (over) {
        g.fillRoundRect(60, 96, 200, 62, 10, th->surface2);
        g.drawRoundRect(60, 96, 200, 62, 10, th->accent);
        g.setTextDatum(MC_DATUM);
        g.setTextFont(2);
        g.setTextColor(th->danger, th->surface2);
        g.drawString("Game Over", 160, 116);
        g.setTextFont(1);
        g.setTextColor(th->text, th->surface2);
        g.drawString("CAM retry   MID menu", 160, 140);
        g.setTextDatum(TL_DATUM);
    }
}

static void runTetris(void)
{
    Drawer g = mkdraw();
    int curT = 0, curR = 0, curX = 3, curY = 0, nextT = 0;
    int score = 0, lines = 0;
    bool over = false;
    uint32_t tickMs = 500, lastTick = millis();

    for (int y = 0; y < TET_ROWS; ++y)
        for (int x = 0; x < TET_COLS; ++x) s_tetBoard[y][x] = 0;
    auto spawn = [&]() {
        curT = nextT; nextT = random(0, 7);
        curR = 0; curX = 3; curY = 0;
        if (tet_collide(curT, curR, curX, curY)) over = true;
        tickMs = (500 - lines * 10 > 120) ? (500 - lines * 10) : 120;
    };
    nextT = random(0, 7);
    spawn();
    while (true) {
        if (g_gameMidReq) { g_gameMidReq = false; bestUpdate(0, score); return; }
        if (takeCamHold() && !over) {          /* 硬降 */
            while (!tet_collide(curT, curR, curX, curY + 1)) ++curY;
            tet_lock(curT, curR, curX, curY);
            lines += tet_clear_lines();
            score += 5;
            spawn();
        }
        if (takeCam()) {
            if (over) {
                for (int y = 0; y < TET_ROWS; ++y)
                    for (int x = 0; x < TET_COLS; ++x) s_tetBoard[y][x] = 0;
                score = 0; lines = 0; over = false; tickMs = 500;
                nextT = random(0, 7); spawn(); lastTick = millis();
            } else {
                int nr = (curR + 1) & 3;
                if (!tet_collide(curT, nr, curX, curY))               curR = nr;
                else if (!tet_collide(curT, nr, curX - 1, curY)) { curR = nr; --curX; }
                else if (!tet_collide(curT, nr, curX + 1, curY)) { curR = nr; ++curX; }
            }
        }
        if (takeUp()   && !over && !tet_collide(curT, curR, curX - 1, curY)) --curX;
        if (takeDown() && !over && !tet_collide(curT, curR, curX + 1, curY)) ++curX;

        if (!over && millis() - lastTick >= tickMs) {
            lastTick = millis();
            if (!tet_collide(curT, curR, curX, curY + 1)) {
                ++curY;
            } else {
                tet_lock(curT, curR, curX, curY);
                int c = tet_clear_lines();
                lines += c;
                score += (c == 1) ? 10 : (c == 2) ? 30 : (c == 3) ? 60 : (c == 4) ? 100 : 0;
                spawn();
            }
        }
        tetDraw(g, curT, curR, curX, curY, nextT, score, lines, over);
        pushFrame(g);
        vTaskDelay(pdMS_TO_TICKS(15));
    }
}

/* =====================================================================
 * 2) 贪吃蛇（16x16，11px 格）
 * =================================================================== */
#define SN_COLS 16
#define SN_ROWS 16
#define SN_CELL 11
#define SN_MAX  200

static uint8_t s_snX[SN_MAX], s_snY[SN_MAX];

static void snDraw(Drawer& g, int len, int score, int fx, int fy, bool over, bool paused)
{
    const Theme* th = g_th;
    const int BX = 72, BY = 32;
    g.fillScreen(th->bg);
    ui_header(g, "Snake", ("score " + String(score)).c_str());

    g.fillRoundRect(BX - 4, BY - 4, SN_COLS * SN_CELL + 8, SN_ROWS * SN_CELL + 8, 8, th->surface2);
    for (int i = 0; i < len; ++i) {
        uint16_t c = (i == 0) ? th->accent : th->ok;
        g.fillRect(BX + s_snX[i] * SN_CELL + 1, BY + s_snY[i] * SN_CELL + 1,
                   SN_CELL - 2, SN_CELL - 2, c);
    }
    g.fillCircle(BX + fx * SN_CELL + SN_CELL / 2, BY + fy * SN_CELL + SN_CELL / 2, 4, th->danger);

    ui_footer(g, "TOP left   CAM right   DN sprint   MID back");

    if (paused) {
        g.fillRoundRect(76, 104, 168, 40, 10, th->surface);
        g.drawRoundRect(76, 104, 168, 40, 10, th->accent);
        g.setTextDatum(MC_DATUM);
        g.setTextFont(2);
        g.setTextSize(1);
        g.setTextColor(th->accent, th->surface);
        g.drawString("CAM to start", 160, 124);
        g.setTextDatum(TL_DATUM);
    }

    if (over) {
        g.fillRoundRect(60, 92, 200, 60, 10, th->surface);
        g.drawRoundRect(60, 92, 200, 60, 10, th->danger);
        g.setTextDatum(MC_DATUM);
        g.setTextFont(2);
        g.setTextSize(1);
        g.setTextColor(th->danger, th->surface);
        g.drawString("Crashed", 160, 112);
        g.setTextFont(1);
        g.setTextColor(th->text, th->surface);
        g.drawString("CAM retry   MID menu", 160, 136);
        g.setTextDatum(TL_DATUM);
    }
}

static void runSnake(void)
{
    Drawer g = mkdraw();
    int len = 4, dir = 1;                 /* 0=上 1=右 2=下 3=左，初始向右 */
    int score = 0;
    int fx = 12, fy = 8;
    bool over = false;
    bool started = false;                 /* 开局先暂停：不然松手 2 秒就撞墙 */
    uint32_t tick = 170, last = millis();

    auto reset = [&]() {
        len = 4; dir = 1; score = 0; over = false; started = false; tick = 170;
        for (int i = 0; i < len; ++i) { s_snX[i] = (uint8_t)(6 - i); s_snY[i] = 8; }
        fx = 12; fy = 8;
        last = millis();
    };
    reset();

    auto placeFood = [&]() {
        for (int tryN = 0; tryN < 200; ++tryN) {
            int nx = random(0, SN_COLS), ny = random(0, SN_ROWS);
            bool hit = false;
            for (int i = 0; i < len; ++i)
                if (s_snX[i] == nx && s_snY[i] == ny) { hit = true; break; }
            if (!hit) { fx = nx; fy = ny; return; }
        }
    };

    while (true) {
        if (g_gameMidReq) { g_gameMidReq = false; bestUpdate(1, score); return; }
        if (takeCam()) {
            if (over)            reset();
            else if (!started)   { started = true; last = millis(); }  /* 起跑 */
            else                 dir = (dir + 1) & 3;                  /* 顺时针 */
        }
        if (takeUp() && !over && started) dir = (dir + 3) & 3;   /* 逆时针 */

        bool sprint = false;
        while (g_gameDown > 0) { --g_gameDown; sprint = true; }

        if (!over && started && millis() - last >= (sprint ? tick / 2 : tick)) {
            last = millis();
            int hx = s_snX[0], hy = s_snY[0];
            if (dir == 0) --hy; else if (dir == 1) ++hx;
            else if (dir == 2) ++hy; else --hx;

            if (hx < 0 || hx >= SN_COLS || hy < 0 || hy >= SN_ROWS) over = true;
            else {
                for (int i = 0; i < len - 1; ++i)
                    if (s_snX[i] == hx && s_snY[i] == hy) { over = true; break; }
            }
            if (!over) {
                bool eat = (hx == fx && hy == fy);
                int newLen = eat && len < SN_MAX ? len + 1 : len;
                for (int i = newLen - 1; i > 0; --i) { s_snX[i] = s_snX[i - 1]; s_snY[i] = s_snY[i - 1]; }
                s_snX[0] = (uint8_t)hx; s_snY[0] = (uint8_t)hy;
                len = newLen;
                if (eat) {
                    score += 10;
                    placeFood();
                    if (tick > 80) tick -= 6;
                }
            }
            if (over) bestUpdate(1, score);
        }
        snDraw(g, len, score, fx, fy, over, !started && !over);
        pushFrame(g);
        vTaskDelay(pdMS_TO_TICKS(15));
    }
}

/* =====================================================================
 * 3) 2048（4x4，方向游标 + CAM 执行）
 * =================================================================== */
static uint16_t s_2048[4][4];

static uint16_t tileColor(int v)
{
    switch (v) {
        case 2:    return 0xCE79;
        case 4:    return 0xDEFB;
        case 8:    return 0xFD20;
        case 16:   return 0xFC00;
        case 32:   return 0xF800;
        case 64:   return 0xE104;
        case 128:  return 0xFFE0;
        case 256:  return 0xFFC0;
        case 512:  return 0xFF80;
        case 1024: return 0x07FF;
        case 2048: return 0x07E0;
        default:   return 0x001F;
    }
}

static uint16_t* cellAddr(int line, int pos, int dir)
{
    switch (dir) {
        case 0: return &s_2048[line][pos];            /* 左 */
        case 2: return &s_2048[line][3 - pos];        /* 右 */
        case 1: return &s_2048[pos][line];            /* 上 */
        default:return &s_2048[3 - pos][line];        /* 下 */
    }
}

static bool move2048(int dir)
{
    bool changed = false;
    for (int line = 0; line < 4; ++line) {
        uint16_t buf[4] = {0, 0, 0, 0};
        int n = 0;
        for (int pos = 0; pos < 4; ++pos) {
            uint16_t v = *cellAddr(line, pos, dir);
            if (v) buf[n++] = v;
        }
        uint16_t out[4] = {0, 0, 0, 0};
        int o = 0;
        for (int i = 0; i < n; ++i) {
            if (i + 1 < n && buf[i] == buf[i + 1]) { out[o++] = (uint16_t)(buf[i] * 2); ++i; }
            else out[o++] = buf[i];
        }
        for (int pos = 0; pos < 4; ++pos) {
            uint16_t* p = cellAddr(line, pos, dir);
            if (*p != out[pos]) { *p = out[pos]; changed = true; }
        }
    }
    return changed;
}

static void spawn2048(void)
{
    int empty[16][2], n = 0;
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            if (!s_2048[y][x]) { empty[n][0] = x; empty[n][1] = y; ++n; }
    if (!n) return;
    int k = random(0, n);
    s_2048[empty[k][1]][empty[k][0]] = (random(0, 10) < 9) ? 2 : 4;
}

static bool canMove2048(void)
{
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            if (!s_2048[y][x]) return true;
            if (x < 3 && s_2048[y][x] == s_2048[y][x + 1]) return true;
            if (y < 3 && s_2048[y][x] == s_2048[y + 1][x]) return true;
        }
    return false;
}

static void draw2048(Drawer& g, int dir, int score, bool over, bool win)
{
    const Theme* th = g_th;
    const int BX = 70, BY = 34, C = 42, GAP = 4;
    g.fillScreen(th->bg);
    ui_header(g, "2048", ("score " + String(score)).c_str());

    g.fillRoundRect(BX - GAP, BY - GAP, 4 * C + 5 * GAP, 4 * C + 5 * GAP, 8, th->surface2);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            int px = BX + x * (C + GAP), py = BY + y * (C + GAP);
            uint16_t v = s_2048[y][x];
            g.fillRoundRect(px, py, C, C, 6, v ? tileColor(v) : th->surface);
            if (v) {
                g.setTextDatum(MC_DATUM);
                g.setTextFont(1);
                g.setTextSize(v < 100 ? 2 : 1);
                g.setTextColor(0x0000, tileColor(v));
                g.drawString(String(v), px + C / 2, py + C / 2);
                g.setTextDatum(TL_DATUM);
            }
        }

    /* 方向游标：左侧竖排四个箭头，亮的那个是当前方向 */
    const char* arrows[4] = { "<", "^", ">", "v" };
    for (int i = 0; i < 4; ++i) {
        int ay = 70 + i * 30;
        g.fillRoundRect(20, ay - 12, 28, 26, 8, (i == dir) ? th->accent : th->surface);
        g.setTextDatum(MC_DATUM);
        g.setTextFont(2);
        g.setTextSize(1);
        g.setTextColor((i == dir) ? th->accentTx : th->muted, (i == dir) ? th->accent : th->surface);
        g.drawString(arrows[i], 34, ay + 1);
        g.setTextDatum(TL_DATUM);
    }

    ui_footer(g, "TOP/DN turn   CAM move   MID back");

    if (over || win) {
        g.fillRoundRect(60, 96, 200, 58, 10, th->surface);
        g.drawRoundRect(60, 96, 200, 58, 10, win ? th->ok : th->danger);
        g.setTextDatum(MC_DATUM);
        g.setTextFont(2);
        g.setTextSize(1);
        g.setTextColor(win ? th->ok : th->danger, th->surface);
        g.drawString(win ? "2048 reached" : "No moves", 160, 114);
        g.setTextFont(1);
        g.setTextColor(th->text, th->surface);
        g.drawString("CAM retry   MID menu", 160, 138);
        g.setTextDatum(TL_DATUM);
    }
}

static void run2048(void)
{
    Drawer g = mkdraw();
    int dir = 0, score = 0;
    bool over = false, win = false;

    auto reset = [&]() {
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) s_2048[y][x] = 0;
        score = 0; over = win = false;
        spawn2048(); spawn2048();
    };
    reset();

    while (true) {
        if (g_gameMidReq) { g_gameMidReq = false; bestUpdate(2, score); return; }
        if (takeCam()) {
            if (over || win) reset();
            else {
                if (move2048(dir)) {
                    spawn2048();
                    for (int y = 0; y < 4 && !win; ++y)
                        for (int x = 0; x < 4; ++x)
                            if (s_2048[y][x] >= 2048) { win = true; break; }
                    if (!canMove2048()) over = true;
                    if (over || win) bestUpdate(2, score);
                }
            }
        }
        if (takeUp())   dir = (dir + 3) & 3;
        if (takeDown()) dir = (dir + 1) & 3;

        draw2048(g, dir, score, over, win);
        pushFrame(g);
        vTaskDelay(pdMS_TO_TICKS(15));
    }
}

/* =====================================================================
 * 入口
 * =================================================================== */
void game_app_loop(void)
{
    bestLoad();
    inputDrain();

    Drawer g = mkdraw();
    int sel = 0;
    menuDraw(g, sel);
    pushFrame(g);

    while (true) {
        if (g_gameMidReq) { g_gameMidReq = false; g_appExitReq = false; return; }
        if (takeUp()) {
            sel = (sel + GAME_COUNT - 1) % GAME_COUNT;
            menuDraw(g, sel); pushFrame(g);
        } else if (takeDown()) {
            sel = (sel + 1) % GAME_COUNT;
            menuDraw(g, sel); pushFrame(g);
        } else if (takeCam()) {
            inputDrain();
            Serial.printf("[GAME] 进入 %s\n", s_gameNames[sel]);
            if (sel == 0)      runTetris();
            else if (sel == 1) runSnake();
            else               run2048();
            inputDrain();
            menuDraw(g, sel); pushFrame(g);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
