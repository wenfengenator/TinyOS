/* SPDX-License-Identifier: GPL-2.0-only */

/* Reversi (Othello) for TinyOS — renders through TinyGL (tinygl.h).
 *
 * Build off-device with musl-gcc (staged as /REVERSI.BIN) or on device:
 *     ./tcc.bin /reversi.c -o /reversi.bin ; ./reversi.bin
 *
 * Play Black against the computer (White), or switch to a two-human game.  The
 * computer uses alpha-beta search with a positional weight table.  Flank the
 * opponent's discs to flip them; a side with no move passes automatically.
 *
 * Controls: arrows / W A S D move the cursor, Space or Enter place a disc,
 * M switch 1P/2P mode, N new game, Q or Esc quit. */
#include <tinygl.h>
#include <unistd.h>
#include <time.h>

#define KEY_UP 0x80
#define KEY_DOWN 0x81
#define KEY_LEFT 0x82
#define KEY_RIGHT 0x83
#define KEY_ESC 0x1B

#define BLACK 1
#define WHITE 2
#define CELL 62
#define MARGIN 16
#define HUD 44
#define BOARD (CELL * 8)
#define AIDEPTH 4

static int board[64], turn, cur, over, aitimer, lastmove, two_player;
static const char *passmsg;

static const int W[64] = {
    120, -20, 20,  5,  5, 20, -20, 120,
    -20, -40, -5, -5, -5, -5, -40, -20,
     20,  -5, 15,  3,  3, 15,  -5,  20,
      5,  -5,  3,  3,  3,  3,  -5,   5,
      5,  -5,  3,  3,  3,  3,  -5,   5,
     20,  -5, 15,  3,  3, 15,  -5,  20,
    -20, -40, -5, -5, -5, -5, -40, -20,
    120, -20, 20,  5,  5, 20, -20, 120,
};

static void text_s(tgl_t *g, int x, int y, const char *s, uint32_t c, int sc) {
    for (; *s; s++, x += 8 * sc) {
        unsigned ch = (unsigned char)*s; if (ch < 0x20 || ch > 0x7E) continue;
        const unsigned char *gl = tgl_font8x8[ch - 0x20];
        for (int row = 0; row < 8; row++) { unsigned bits = gl[row];
            for (int col = 0; col < 8; col++) if (bits & (1u << col)) tgl_fill(g, x + col * sc, y + row * sc, sc, sc, c); }
    }
}
static void text_c(tgl_t *g, int y, const char *s, uint32_t c, int sc) {
    int n = 0; for (const char *p = s; *p; p++) n++;
    text_s(g, g->w / 2 - n * 8 * sc / 2, y, s, c, sc);
}
static int num(char *b, long v) {
    char t[8]; int i = 0; if (v == 0) t[i++] = '0';
    while (v) { t[i++] = (char)('0' + v % 10); v /= 10; }
    int j = 0; while (i) b[j++] = t[--i]; b[j] = 0; return j;
}
static int isqrt(int v) { if (v < 0) return 0; int x = 0; while ((x + 1) * (x + 1) <= v) x++; return x; }
static void disc(tgl_t *g, int cx, int cy, int r, uint32_t c) {
    for (int dy = -r; dy <= r; dy++) { int w = isqrt(r * r - dy * dy); tgl_fill(g, cx - w, cy + dy, 2 * w + 1, 1, c); }
}

/* count (and optionally flip) discs flanked by placing `who` at `cell`. */
static int flip(int *bd, int cell, int who, int apply) {
    if (bd[cell]) return 0;
    int opp = 3 - who, total = 0, r = cell / 8, c = cell % 8;
    for (int dr = -1; dr <= 1; dr++) for (int dc = -1; dc <= 1; dc++) {
        if (!dr && !dc) continue;
        int rr = r + dr, cc = c + dc, cnt = 0;
        while (rr >= 0 && rr < 8 && cc >= 0 && cc < 8 && bd[rr * 8 + cc] == opp) { rr += dr; cc += dc; cnt++; }
        if (cnt && rr >= 0 && rr < 8 && cc >= 0 && cc < 8 && bd[rr * 8 + cc] == who) {
            total += cnt;
            if (apply) { int r2 = r + dr, c2 = c + dc; for (int k = 0; k < cnt; k++) { bd[r2 * 8 + c2] = who; r2 += dr; c2 += dc; } }
        }
    }
    return total;
}
static int has_move(int *bd, int who) { for (int i = 0; i < 64; i++) if (!bd[i] && flip(bd, i, who, 0)) return 1; return 0; }
static void do_move(int *bd, int cell, int who) { flip(bd, cell, who, 1); bd[cell] = who; }
static int evalb(int *bd) {
    int s = 0; for (int i = 0; i < 64; i++) { if (bd[i] == WHITE) s += W[i]; else if (bd[i] == BLACK) s -= W[i]; }
    return s;
}
/* alpha-beta; WHITE maximizes, BLACK minimizes. */
static int search(int *bd, int who, int depth, int alpha, int beta) {
    if (depth == 0) return evalb(bd);
    int any = 0, best = (who == WHITE) ? -1000000 : 1000000;
    for (int i = 0; i < 64; i++) {
        if (bd[i] || flip(bd, i, who, 0) == 0) continue;
        any = 1;
        int nb[64]; for (int k = 0; k < 64; k++) nb[k] = bd[k];
        do_move(nb, i, who);
        int v = search(nb, 3 - who, depth - 1, alpha, beta);
        if (who == WHITE) { if (v > best) best = v; if (best > alpha) alpha = best; }
        else { if (v < best) best = v; if (best < beta) beta = best; }
        if (beta <= alpha) break;
    }
    if (!any) {
        if (!has_move(bd, 3 - who)) {
            int w = 0, b = 0; for (int i = 0; i < 64; i++) { if (bd[i] == WHITE) w++; else if (bd[i] == BLACK) b++; }
            return (w - b) * 1000;
        }
        return search(bd, 3 - who, depth - 1, alpha, beta);
    }
    return best;
}
static int ai_best(int *bd) {
    int best = -2000000, bm = -1;
    for (int i = 0; i < 64; i++) {
        if (bd[i] || flip(bd, i, WHITE, 0) == 0) continue;
        int nb[64]; for (int k = 0; k < 64; k++) nb[k] = bd[k];
        do_move(nb, i, WHITE);
        int v = search(nb, BLACK, AIDEPTH - 1, -2000000, 2000000);
        if (v > best || bm < 0) { best = v; bm = i; }
    }
    return bm;
}
static void new_game(void) {
    for (int i = 0; i < 64; i++) board[i] = 0;
    board[27] = WHITE; board[28] = BLACK; board[35] = BLACK; board[36] = WHITE;
    turn = BLACK; cur = 26; over = 0; aitimer = 0; lastmove = -1; passmsg = 0;
}

static void draw(tgl_t *g) {
    tgl_vgradient(g, 0, 0, g->w, g->h, 0x0B1E14, 0x0E2A1C);
    int BX = MARGIN, BY = HUD;
    tgl_fill(g, BX - 5, BY - 5, BOARD + 10, BOARD + 10, 0x0A3320);
    for (int i = 0; i < 64; i++) {
        int r = i / 8, c = i % 8, x = BX + c * CELL, y = BY + r * CELL;
        tgl_fill(g, x, y, CELL, CELL, ((r ^ c) & 1) ? 0x1B7A44 : 0x1E8A4C);
        tgl_blend(g, x, y, CELL, 1, 0x000000, 60); tgl_blend(g, x, y, 1, CELL, 0x000000, 60);
        if (board[i]) {
            uint32_t col = board[i] == BLACK ? 0x1A1D24 : 0xF2F4F8;
            disc(g, x + CELL / 2, y + CELL / 2, CELL / 2 - 6, col);
            disc(g, x + CELL / 2 - 4, y + CELL / 2 - 5, 3, board[i] == BLACK ? 0x50586A : 0xFFFFFF);
        } else if (!over && (two_player || turn == BLACK) && flip(board, i, turn, 0)) {
            disc(g, x + CELL / 2, y + CELL / 2, 5, 0x8CF0B0);
        }
        if (i == lastmove) { tgl_blend(g, x + 2, y + 2, CELL - 4, 2, 0xFFE060, 160); }
    }
    if (!over && (two_player || turn == BLACK)) {    /* cursor box */
        int x = BX + (cur % 8) * CELL, y = BY + (cur / 8) * CELL;
        tgl_fill(g, x, y, CELL, 3, 0xFFE060); tgl_fill(g, x, y + CELL - 3, CELL, 3, 0xFFE060);
        tgl_fill(g, x, y, 3, CELL, 0xFFE060); tgl_fill(g, x + CELL - 3, y, 3, CELL, 0xFFE060);
    }
    int cb = 0, cw = 0; for (int i = 0; i < 64; i++) { if (board[i] == BLACK) cb++; else if (board[i] == WHITE) cw++; }
    text_s(g, MARGIN, 12, "REVERSI", 0xEAF2FF, 3);
    char b[8];
    disc(g, g->w - 150, 20, 9, 0x1A1D24); num(b, cb); text_s(g, g->w - 136, 12, b, 0xEAF2FF, 2);
    disc(g, g->w - 92, 20, 9, 0xF2F4F8); num(b, cw); text_s(g, g->w - 78, 12, b, 0xEAF2FF, 2);
    const char *st = over ? "" : (turn == BLACK ? "BLACK TO MOVE" : (two_player ? "WHITE TO MOVE" : "WHITE THINKING..."));
    text_s(g, 220, 12, two_player ? "2 PLAYERS" : "VS CPU", 0x9CC7AC, 1);
    text_s(g, MARGIN, BY + BOARD + 10, passmsg ? passmsg : st, 0x9CC7AC, 1);
    text_s(g, MARGIN, BY + BOARD + 26, "arrows/wasd MOVE  space PLACE  M MODE  N NEW  Q QUIT", 0x6E86AC, 1);
    if (over) {
        tgl_blend(g, 0, 0, g->w, g->h, 0x000000, 150);
        const char *r = cb > cw ? (two_player ? "BLACK WINS" : "YOU WIN!") : (cw > cb ? "WHITE WINS" : "A DRAW");
        text_c(g, g->h / 2 - 30, r, cb > cw ? 0x60F0B0 : 0xFF8080, 3);
        text_c(g, g->h / 2 + 14, "press N for a new game", 0xEAF2FF, 1);
    }
}

int main(void) {
    int sw = 1024, sh = 768; tgl_screen_size(&sw, &sh);
    int w = MARGIN * 2 + BOARD, h = HUD + BOARD + 46;
    tgl_t g;
    if (tgl_open(&g, w, h) != 0) { static const char *e = "reversi: no display\n"; write(2, e, 20); return 1; }
    int ox = (sw - g.w) / 2, oy = (sh - g.h) / 2; if (ox < 0) ox = 0; if (oy < 0) oy = 0;
    new_game();
    for (;;) {
        if (!over) {
            int hb = has_move(board, BLACK), hw = has_move(board, WHITE);
            if (!hb && !hw) over = 1;
            else if (turn == BLACK && !hb) { turn = WHITE; aitimer = 16; passmsg = "Black has no move - pass"; }
            else if (turn == WHITE && !hw) { turn = BLACK; passmsg = "White has no move - pass"; }
        }
        draw(&g);
        int key = tgl_present(&g, ox, oy);
        if (key == 'q' || key == 'Q' || key == KEY_ESC) break;
        if (key == 'n' || key == 'N') { new_game(); usleep(16000); continue; }
        if (key == 'm' || key == 'M') { two_player = !two_player; new_game(); usleep(16000); continue; }
        if (!over && (two_player || turn == BLACK)) {
            if (key == KEY_UP || key == 'w' || key == 'W') { if (cur >= 8) cur -= 8; }
            else if (key == KEY_DOWN || key == 's' || key == 'S') { if (cur < 56) cur += 8; }
            else if (key == KEY_LEFT || key == 'a' || key == 'A') { if (cur % 8) cur--; }
            else if (key == KEY_RIGHT || key == 'd' || key == 'D') { if (cur % 8 != 7) cur++; }
            else if (key == ' ' || key == '\n' || key == '\r') {
                if (flip(board, cur, turn, 0)) { do_move(board, cur, turn); lastmove = cur; turn = 3 - turn; aitimer = 16; passmsg = 0; }
            }
        }
        if (!over && !two_player && turn == WHITE) {
            if (aitimer > 0) aitimer--;
            else { int m = ai_best(board); if (m >= 0) { do_move(board, m, WHITE); lastmove = m; } turn = BLACK; passmsg = 0; }
        }
        usleep(16000);
    }
    tgl_finish(&g);
    return 0;
}
