/* SPDX-License-Identifier: GPL-2.0-only */

/* Klondike Solitaire for TinyOS, rendered through TinyGL.
 *
 * Build off-device with musl-gcc (staged as /SOLITAIR.BIN) or on device:
 *     ./tcc.bin /solitaire.c -o /solitaire.bin ; ./solitaire.bin
 *
 * Controls: arrows move between piles/cards, Space or Enter selects and moves,
 * A sends an exposed card to its foundation, N deals again, Q or Esc quits. */
#include <tinygl.h>
#include <unistd.h>
#include <time.h>
#include <string.h>

#define KEY_UP 0x80
#define KEY_DOWN 0x81
#define KEY_LEFT 0x82
#define KEY_RIGHT 0x83
#define KEY_ESC 0x1B

#define CW 72
#define CH 96
#define GAP 18
#define X0 22
#define TOP_Y 58
#define TAB_Y 180
#define WIN_W (X0 * 2 + CW * 7 + GAP * 6)
#define WIN_H 650

typedef struct { unsigned char id, up; } Card;

enum { Z_TOP, Z_TABLEAU };
enum { SEL_NONE, SEL_WASTE, SEL_FOUNDATION, SEL_TABLEAU };

static int stock[52], waste[52], stock_n, waste_n;
static Card tab[7][52];
static int tab_n[7], foundation[4];
static int zone, col, tab_cursor[7];
static int sel_kind, sel_col, sel_index;
static int won, moves;
static const char *message;
static unsigned long rng;

static int rankof(int card) { return card % 13 + 1; }
static int suitof(int card) { return card / 13; }
static int red(int card) { int suit = suitof(card); return suit == 1 || suit == 2; }
static unsigned rnd(void) {
    if (!rng) rng = ((unsigned long)time(0) << 1) | 1;
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (unsigned)(rng >> 5);
}
static void shuffle(int *cards, int n) {
    for (int i = n - 1; i > 0; i--) {
        int j = (int)(rnd() % (unsigned)(i + 1));
        int card = cards[i]; cards[i] = cards[j]; cards[j] = card;
    }
}
static void text_s(tgl_t *g, int x, int y, const char *s, uint32_t color, int scale) {
    for (; *s; s++, x += 8 * scale) {
        unsigned ch = (unsigned char)*s;
        if (ch < 0x20 || ch > 0x7e) continue;
        const unsigned char *glyph = tgl_font8x8[ch - 0x20];
        for (int row = 0; row < 8; row++) for (int bit = 0; bit < 8; bit++)
            if (glyph[row] & (1u << bit))
                tgl_fill(g, x + bit * scale, y + row * scale, scale, scale, color);
    }
}
static void text_c(tgl_t *g, int y, const char *s, uint32_t color, int scale) {
    int length = 0; for (const char *p = s; *p; p++) length++;
    text_s(g, g->w / 2 - length * 8 * scale / 2, y, s, color, scale);
}
static int pile_x(int pile) { return X0 + pile * (CW + GAP); }
static void outline(tgl_t *g, int x, int y, int w, int h, uint32_t color, int thick) {
    tgl_fill(g, x, y, w, thick, color); tgl_fill(g, x, y + h - thick, w, thick, color);
    tgl_fill(g, x, y, thick, h, color); tgl_fill(g, x + w - thick, y, thick, h, color);
}
static void card_label(int card, char label[4]) {
    int rank = rankof(card), n = 0;
    if (rank == 1) label[n++] = 'A';
    else if (rank == 10) { label[n++] = '1'; label[n++] = '0'; }
    else if (rank == 11) label[n++] = 'J';
    else if (rank == 12) label[n++] = 'Q';
    else if (rank == 13) label[n++] = 'K';
    else label[n++] = (char)('0' + rank);
    label[n] = 0;
}
static void draw_back(tgl_t *g, int x, int y) {
    tgl_fill(g, x, y, CW, CH, 0xE9EEF3); tgl_fill(g, x + 4, y + 4, CW - 8, CH - 8, 0x1F4F8B);
    for (int yy = y + 8; yy < y + CH - 8; yy += 8)
        for (int xx = x + 8 + ((yy / 8) & 1) * 4; xx < x + CW - 8; xx += 8)
            tgl_fill(g, xx, yy, 3, 3, 0xA9C7E8);
}
static void draw_card(tgl_t *g, int card, int x, int y) {
    uint32_t ink = red(card) ? 0xC52D3A : 0x17202B;
    tgl_fill(g, x, y, CW, CH, 0xF7F5EE); outline(g, x, y, CW, CH, 0xC9C7BE, 2);
    char label[4]; card_label(card, label); text_s(g, x + 7, y + 7, label, ink, 2);
    char suit[2] = { "CDHS"[suitof(card)], 0 };
    text_s(g, x + 8, y + 31, suit, ink, 2);
    text_s(g, x + CW / 2 - 8, y + CH / 2 - 8, suit, ink, 2);
}
static int tableau_y(int pile, int index) {
    int y = TAB_Y, gap = tab_n[pile] > 1 ? 310 / (tab_n[pile] - 1) : 26;
    if (gap > 26) gap = 26;
    if (gap < 12) gap = 12;
    for (int i = 0; i < index; i++) y += tab[pile][i].up ? gap : (gap < 15 ? gap : 15);
    return y;
}
static int first_up(int pile) {
    for (int i = 0; i < tab_n[pile]; i++) if (tab[pile][i].up) return i;
    return tab_n[pile];
}
static int valid_run(int pile, int index) {
    if (index < 0 || index >= tab_n[pile] || !tab[pile][index].up) return 0;
    for (int i = index; i + 1 < tab_n[pile]; i++) {
        int upper = tab[pile][i].id, lower = tab[pile][i + 1].id;
        if (!tab[pile][i + 1].up || red(upper) == red(lower) || rankof(upper) != rankof(lower) + 1) return 0;
    }
    return 1;
}
static int can_tableau(int card, int pile) {
    if (!tab_n[pile]) return rankof(card) == 13;
    Card top = tab[pile][tab_n[pile] - 1];
    return top.up && red(top.id) != red(card) && rankof(top.id) == rankof(card) + 1;
}
static int can_foundation(int card, int pile) {
    return suitof(card) == pile && rankof(card) == foundation[pile] + 1;
}
static void reveal(int pile) {
    if (tab_n[pile] && !tab[pile][tab_n[pile] - 1].up) tab[pile][tab_n[pile] - 1].up = 1;
    tab_cursor[pile] = tab_n[pile] ? tab_n[pile] - 1 : 0;
}
static void clear_selection(void) { sel_kind = SEL_NONE; sel_col = sel_index = -1; }
static void check_win(void) {
    won = foundation[0] == 13 && foundation[1] == 13 && foundation[2] == 13 && foundation[3] == 13;
    if (won) message = "YOU WON - press N to deal again";
}
static void new_game(void) {
    int deck[52]; for (int i = 0; i < 52; i++) deck[i] = i; shuffle(deck, 52);
    memset(tab_n, 0, sizeof tab_n); memset(foundation, 0, sizeof foundation);
    stock_n = waste_n = 0; int next = 0;
    for (int pile = 0; pile < 7; pile++) {
        for (int row = 0; row <= pile; row++) {
            tab[pile][tab_n[pile]].id = (unsigned char)deck[next++];
            tab[pile][tab_n[pile]].up = (unsigned char)(row == pile);
            tab_n[pile]++;
        }
        tab_cursor[pile] = tab_n[pile] - 1;
    }
    while (next < 52) stock[stock_n++] = deck[next++];
    zone = Z_TABLEAU; col = 0; moves = won = 0; message = "Build down in alternating colors"; clear_selection();
}
static void draw_stock(void) {
    clear_selection();
    if (stock_n) { waste[waste_n++] = stock[--stock_n]; moves++; message = "Drew from stock"; }
    else if (waste_n) {
        while (waste_n) stock[stock_n++] = waste[--waste_n];
        moves++; message = "Stock recycled";
    } else message = "Stock is empty";
}
static int selected_card(void) {
    if (sel_kind == SEL_WASTE) return waste_n ? waste[waste_n - 1] : -1;
    if (sel_kind == SEL_FOUNDATION) return foundation[sel_col] ? sel_col * 13 + foundation[sel_col] - 1 : -1;
    if (sel_kind == SEL_TABLEAU && sel_index < tab_n[sel_col]) return tab[sel_col][sel_index].id;
    return -1;
}
static void choose_source(void) {
    if (zone == Z_TOP && col == 0) { draw_stock(); return; }
    if (zone == Z_TOP && col == 1 && waste_n) { sel_kind = SEL_WASTE; sel_col = 1; sel_index = waste_n - 1; }
    else if (zone == Z_TOP && col >= 3 && foundation[col - 3]) { sel_kind = SEL_FOUNDATION; sel_col = col - 3; sel_index = foundation[sel_col] - 1; }
    else if (zone == Z_TABLEAU && tab_n[col] && valid_run(col, tab_cursor[col])) {
        sel_kind = SEL_TABLEAU; sel_col = col; sel_index = tab_cursor[col];
    } else { message = "No movable card here"; return; }
    message = "Card selected - choose destination";
}
static void remove_selected(void) {
    if (sel_kind == SEL_WASTE) waste_n--;
    else if (sel_kind == SEL_FOUNDATION) foundation[sel_col]--;
    else if (sel_kind == SEL_TABLEAU) { tab_n[sel_col] = sel_index; reveal(sel_col); }
}
static int drop_tableau(int pile) {
    int card = selected_card(); if (card < 0 || !can_tableau(card, pile)) return 0;
    if (sel_kind == SEL_TABLEAU) {
        int count = tab_n[sel_col] - sel_index;
        if (sel_col == pile) return 0;
        for (int i = 0; i < count; i++) tab[pile][tab_n[pile]++] = tab[sel_col][sel_index + i];
    } else { tab[pile][tab_n[pile]].id = (unsigned char)card; tab[pile][tab_n[pile]++].up = 1; }
    remove_selected(); tab_cursor[pile] = tab_n[pile] - 1; return 1;
}
static int drop_foundation(int pile) {
    int card = selected_card();
    if (card < 0 || sel_kind == SEL_FOUNDATION || (sel_kind == SEL_TABLEAU && sel_index != tab_n[sel_col] - 1)) return 0;
    if (!can_foundation(card, pile)) return 0;
    foundation[pile]++; remove_selected(); return 1;
}
static void activate(void) {
    if (won) return;
    if (sel_kind == SEL_NONE) { choose_source(); return; }
    if ((zone == Z_TOP && ((sel_kind == SEL_WASTE && col == 1) ||
        (sel_kind == SEL_FOUNDATION && col == sel_col + 3))) ||
        (zone == Z_TABLEAU && sel_kind == SEL_TABLEAU && col == sel_col && tab_cursor[col] == sel_index)) {
        clear_selection(); message = "Selection cancelled"; return;
    }
    int moved = zone == Z_TABLEAU ? drop_tableau(col) : (col >= 3 ? drop_foundation(col - 3) : 0);
    if (moved) { moves++; message = "Move complete"; clear_selection(); check_win(); }
    else message = "That move is not allowed";
}
static void auto_foundation(void) {
    if (won) return;
    int card = -1, source = SEL_NONE, source_col = -1;
    if (zone == Z_TOP && col == 1 && waste_n) { card = waste[waste_n - 1]; source = SEL_WASTE; }
    else if (zone == Z_TABLEAU && tab_n[col] && tab_cursor[col] == tab_n[col] - 1 && tab[col][tab_n[col] - 1].up) {
        card = tab[col][tab_n[col] - 1].id; source = SEL_TABLEAU; source_col = col;
    }
    if (card < 0 || !can_foundation(card, suitof(card))) { message = "Card cannot go to foundation"; return; }
    clear_selection(); sel_kind = source; sel_col = source_col; sel_index = source == SEL_TABLEAU ? tab_n[source_col] - 1 : waste_n - 1;
    foundation[suitof(card)]++; remove_selected(); clear_selection(); moves++; message = "Moved to foundation"; check_win();
}
static int top_valid_col(int candidate, int direction) {
    if (candidate == 2) candidate += direction;
    if (candidate < 0) candidate = 6;
    if (candidate > 6) candidate = 0;
    if (candidate == 2) candidate += direction;
    return candidate;
}
static void move_cursor(int key) {
    if (key == KEY_LEFT) col = zone == Z_TOP ? top_valid_col(col - 1, -1) : (col + 6) % 7;
    else if (key == KEY_RIGHT) col = zone == Z_TOP ? top_valid_col(col + 1, 1) : (col + 1) % 7;
    else if (key == KEY_UP) {
        if (zone == Z_TABLEAU && tab_n[col] && tab_cursor[col] > first_up(col)) tab_cursor[col]--;
        else { zone = Z_TOP; if (col == 2) col = 3; }
    } else if (key == KEY_DOWN) {
        if (zone == Z_TOP) zone = Z_TABLEAU;
        else if (tab_n[col] && tab_cursor[col] < tab_n[col] - 1) tab_cursor[col]++;
    }
}
static void draw_empty(tgl_t *g, int x, int y, const char *label) {
    outline(g, x, y, CW, CH, 0x4A9A72, 2); text_s(g, x + CW / 2 - 4, y + CH / 2 - 4, label, 0x69B98C, 1);
}
static void draw_game(tgl_t *g) {
    tgl_vgradient(g, 0, 0, g->w, g->h, 0x0B492F, 0x073522);
    text_s(g, X0, 15, "SOLITAIRE", 0xF4E6B5, 3);
    char move_text[16] = "MOVES "; int p = 6, value = moves, digits = 1;
    for (int n = value; n >= 10; n /= 10) digits++;
    for (int i = digits - 1; i >= 0; i--) { move_text[p + i] = (char)('0' + value % 10); value /= 10; }
    move_text[p + digits] = 0; text_s(g, g->w - 130, 20, move_text, 0xD3E7DA, 1);

    if (stock_n) draw_back(g, pile_x(0), TOP_Y); else draw_empty(g, pile_x(0), TOP_Y, "R");
    if (waste_n) draw_card(g, waste[waste_n - 1], pile_x(1), TOP_Y); else draw_empty(g, pile_x(1), TOP_Y, "W");
    for (int suit = 0; suit < 4; suit++) {
        int x = pile_x(suit + 3);
        if (foundation[suit]) draw_card(g, suit * 13 + foundation[suit] - 1, x, TOP_Y);
        else { char s[2] = { "CDHS"[suit], 0 }; draw_empty(g, x, TOP_Y, s); }
    }
    for (int pile = 0; pile < 7; pile++) {
        if (!tab_n[pile]) draw_empty(g, pile_x(pile), TAB_Y, "K");
        for (int i = 0; i < tab_n[pile]; i++) {
            int y = tableau_y(pile, i);
            if (tab[pile][i].up) draw_card(g, tab[pile][i].id, pile_x(pile), y); else draw_back(g, pile_x(pile), y);
        }
    }
    int cursor_y = TOP_Y;
    if (zone == Z_TABLEAU) cursor_y = tab_n[col] ? tableau_y(col, tab_cursor[col]) : TAB_Y;
    outline(g, pile_x(col) - 4, cursor_y - 4, CW + 8, CH + 8, 0xFFD55A, 3);
    if (sel_kind != SEL_NONE) {
        int sx, sy;
        if (sel_kind == SEL_WASTE) { sx = pile_x(1); sy = TOP_Y; }
        else if (sel_kind == SEL_FOUNDATION) { sx = pile_x(sel_col + 3); sy = TOP_Y; }
        else { sx = pile_x(sel_col); sy = tableau_y(sel_col, sel_index); }
        outline(g, sx - 2, sy - 2, CW + 4, CH + 4, 0x62D6FF, 3);
    }
    text_s(g, X0, WIN_H - 48, message, 0xE6F1E9, 1);
    text_s(g, X0, WIN_H - 27, "ARROWS MOVE  SPACE SELECT/DROP  A FOUNDATION  N NEW  Q QUIT", 0x9AC9AE, 1);
    if (won) { tgl_blend(g, 0, 0, g->w, g->h, 0x00150D, 165); text_c(g, 270, "YOU WON!", 0xFFD55A, 4); text_c(g, 320, "press N for a new deal", 0xFFFFFF, 1); }
}
static int selftest(void) {
    rng = 0x123456789UL; new_game();
    int seen[52]; memset(seen, 0, sizeof seen); int count = 0;
    for (int pile = 0; pile < 7; pile++) for (int i = 0; i < tab_n[pile]; i++) { seen[tab[pile][i].id]++; count++; }
    for (int i = 0; i < stock_n; i++) { seen[stock[i]]++; count++; }
    if (count != 52 || stock_n != 24) return 1;
    for (int i = 0; i < 52; i++) if (seen[i] != 1) return 2;
    tab_n[0] = 0;
    if (!can_tableau(12, 0) || can_tableau(11, 0)) return 3;
    tab_n[0] = 1; tab[0][0].id = 12; tab[0][0].up = 1;
    if (!can_tableau(24, 0) || can_tableau(11, 0)) return 4;
    memset(foundation, 0, sizeof foundation);
    if (!can_foundation(0, 0) || can_foundation(1, 0)) return 5;
    foundation[0] = 1; if (!can_foundation(1, 0)) return 6;
    tgl_t graphics;
    memset(&graphics, 0, sizeof graphics);
    graphics.w = graphics.stride = WIN_W; graphics.h = WIN_H;
    graphics.rsh = 16; graphics.gsh = 8; graphics.bsh = 0;
    graphics.px = calloc((size_t)WIN_W * WIN_H, sizeof *graphics.px);
    if (!graphics.px) return 7;
    draw_game(&graphics);
    int painted = 0;
    for (int i = 0; i < WIN_W * WIN_H; i++) if (graphics.px[i]) painted++;
    free(graphics.px);
    if (painted < WIN_W * WIN_H / 2) return 8;
    return 0;
}
int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--selftest")) return selftest();
    int screen_w = 1024, screen_h = 768; tgl_screen_size(&screen_w, &screen_h);
    tgl_t graphics;
    if (tgl_open(&graphics, WIN_W, WIN_H) != 0) { write(2, "solitaire: no display\n", 22); return 1; }
    int ox = (screen_w - graphics.w) / 2, oy = (screen_h - graphics.h) / 2;
    if (ox < 0) ox = 0;
    if (oy < 0) oy = 0;
    new_game();
    for (;;) {
        draw_game(&graphics);
        int key = tgl_present(&graphics, ox, oy);
        if (key == 'q' || key == 'Q' || key == KEY_ESC) break;
        if (key == 'n' || key == 'N') new_game();
        else if (key == 'a' || key == 'A') auto_foundation();
        else if (key == ' ' || key == '\n' || key == '\r') activate();
        else if (key == KEY_UP || key == KEY_DOWN || key == KEY_LEFT || key == KEY_RIGHT) move_cursor(key);
        usleep(16000);
    }
    tgl_finish(&graphics);
    return 0;
}
