/* SPDX-License-Identifier: GPL-2.0-only */

/* TinyDoom: a clean-room Doom-style vertical slice for TinyOS.
 * No Doom source code or copyrighted game assets are used. */
#include <tinygl.h>
#include <taudio.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define KEY_UP    0x80
#define KEY_DOWN  0x81
#define KEY_LEFT  0x82
#define KEY_RIGHT 0x83
#define KEY_ESC   0x1B

#define WIDTH 640
#define HEIGHT 400
#define VIEW_H 344
#define MAP_W 21
#define MAP_H 16
#define TILE 256
#define MAX_ENEMIES 8
#define RAY_STEP 8
#define MAX_RAY (MAP_W * TILE)

typedef struct { int x, y, hp, alive, cooldown; } Enemy;

static const char *LEVEL[MAP_H] = {
    "#####################",
    "#......#...........X#",
    "#.####.#.#####.###..#",
    "#.#....#.....#...#..#",
    "#.#.######.#.###.#..#",
    "#.#........#.....#..#",
    "#.######.#######.#..#",
    "#........#.....#.#..#",
    "####.#####.###.#.#..#",
    "#....#.....#...#....#",
    "#.####.#####.#####..#",
    "#.#....#........E...#",
    "#.#.####.#####.###..#",
    "#...E....#...A.....H#",
    "#P.......#..........#",
    "#####################",
};

static char map_data[MAP_H][MAP_W + 1];
static Enemy enemies[MAX_ENEMIES];
static int enemy_count;
static int px, py, angle, health, ammo, kills, total_enemies;
static int won, dead, flash, muzzle, sound_ready;
static int wall_depth[WIDTH];

static int iabs(int value) { return value < 0 ? -value : value; }
static int wall_at(int x, int y) {
    int mx = x / TILE, my = y / TILE;
    return mx < 0 || mx >= MAP_W || my < 0 || my >= MAP_H || map_data[my][mx] == '#';
}
static int ray_distance(int origin_x, int origin_y, int ray_angle) {
    int cosine = tgl_icos(ray_angle), sine = tgl_isin(ray_angle);
    for (int distance = RAY_STEP; distance < MAX_RAY; distance += RAY_STEP) {
        int x = origin_x + (int)(((int64_t)cosine * distance) >> 16);
        int y = origin_y + (int)(((int64_t)sine * distance) >> 16);
        if (wall_at(x, y)) return distance;
    }
    return MAX_RAY;
}
static int line_clear(int x0, int y0, int x1, int y1) {
    int dx = x1 - x0, dy = y1 - y0;
    int distance = iabs(dx) > iabs(dy) ? iabs(dx) : iabs(dy);
    if (!distance) return 1;
    for (int step = 16; step < distance; step += 16)
        if (wall_at(x0 + dx * step / distance, y0 + dy * step / distance)) return 0;
    return 1;
}

static void load_level(void) {
    enemy_count = 0;
    for (int y = 0; y < MAP_H; y++) {
        memcpy(map_data[y], LEVEL[y], MAP_W + 1);
        for (int x = 0; x < MAP_W; x++) {
            char tile = map_data[y][x];
            if (tile == 'P') { px = x * TILE + TILE / 2; py = y * TILE + TILE / 2; angle = 192; map_data[y][x] = '.'; }
            if (tile == 'E' && enemy_count < MAX_ENEMIES) {
                enemies[enemy_count++] = (Enemy){ x * TILE + TILE / 2, y * TILE + TILE / 2, 2, 1, 0 };
                map_data[y][x] = '.';
            }
        }
    }
    total_enemies = enemy_count; health = 100; ammo = 24; kills = 0;
    won = dead = flash = muzzle = 0;
}

static void text(tgl_t *g, int x, int y, const char *s, uint32_t color, int scale) {
    for (; *s; s++, x += 8 * scale) {
        unsigned ch = (unsigned char)*s;
        if (ch < 0x20 || ch > 0x7e) continue;
        const unsigned char *glyph = tgl_font8x8[ch - 0x20];
        for (int row = 0; row < 8; row++)
            for (int col = 0; col < 8; col++)
                if (glyph[row] & (1u << col)) tgl_fill(g, x + col * scale, y + row * scale, scale, scale, color);
    }
}
static void number(char *out, int value) {
    char reverse[12]; int count = 0, at = 0;
    if (value < 0) { out[at++] = '-'; value = -value; }
    do { reverse[count++] = (char)('0' + value % 10); value /= 10; } while (value);
    while (count) out[at++] = reverse[--count];
    out[at] = 0;
}
static uint32_t shade(uint32_t color, int light) {
    int red = ((color >> 16) & 255) * light / 255;
    int green = ((color >> 8) & 255) * light / 255;
    int blue = (color & 255) * light / 255;
    return (uint32_t)(red << 16 | green << 8 | blue);
}

static void render_world(tgl_t *g) {
    tgl_vgradient(g, 0, 0, WIDTH, VIEW_H / 2, 0x101528, 0x384058);
    tgl_vgradient(g, 0, VIEW_H / 2, WIDTH, VIEW_H / 2, 0x38342e, 0x11100f);
    for (int column = 0; column < WIDTH; column++) {
        int offset = (column - WIDTH / 2) * 64 / WIDTH;
        int distance = ray_distance(px, py, angle + offset);
        distance = (int)(((int64_t)distance * tgl_icos(offset)) >> 16);
        if (distance < 1) distance = 1;
        wall_depth[column] = distance;
        int height = 56000 / distance;
        if (height > VIEW_H) height = VIEW_H;
        int top = VIEW_H / 2 - height / 2;
        int mx = px + (int)(((int64_t)tgl_icos(angle + offset) * distance) >> 16);
        int my = py + (int)(((int64_t)tgl_isin(angle + offset) * distance) >> 16);
        int checker = ((mx / 32) ^ (my / 32)) & 1;
        int light = 255 - distance / 16; if (light < 55) light = 55;
        uint32_t base = checker ? 0x8d3329 : 0x6e241f;
        tgl_fill(g, column, top, 1, height, shade(base, light));
    }
}

static void draw_enemy(tgl_t *g, Enemy *enemy) {
    if (!enemy->alive) return;
    int dx = enemy->x - px, dy = enemy->y - py;
    int forward = (int)(((int64_t)dx * tgl_icos(angle) + (int64_t)dy * tgl_isin(angle)) >> 16);
    int side = (int)((-(int64_t)dx * tgl_isin(angle) + (int64_t)dy * tgl_icos(angle)) >> 16);
    if (forward < 32) return;
    int center = WIDTH / 2 + side * 420 / forward;
    int size = 42000 / forward; if (size > 210) size = 210; if (size < 4) return;
    int left = center - size / 2, top = VIEW_H / 2 - size / 2;
    for (int x = 0; x < size; x++) {
        int sx = left + x; if ((unsigned)sx >= WIDTH || wall_depth[sx] < forward) continue;
        int body_left = size / 5, body_right = size - body_left;
        if (x < body_left || x > body_right) continue;
        uint32_t color = enemy->hp == 1 ? 0xd04732 : 0x8f261f;
        tgl_fill(g, sx, top + size / 4, 1, size * 3 / 4, color);
        if (x > size / 3 && x < size * 2 / 3) tgl_fill(g, sx, top, 1, size / 3, 0xb66b43);
        if ((x == size * 2 / 5 || x == size * 3 / 5) && size > 18)
            tgl_fill(g, sx, top + size / 8, 1, size / 14 + 1, 0xffd030);
    }
}

static void render_sprites(tgl_t *g) {
    int order[MAX_ENEMIES];
    for (int i = 0; i < enemy_count; i++) order[i] = i;
    for (int i = 0; i < enemy_count; i++) for (int j = i + 1; j < enemy_count; j++) {
        int ai = iabs(enemies[order[i]].x - px) + iabs(enemies[order[i]].y - py);
        int aj = iabs(enemies[order[j]].x - px) + iabs(enemies[order[j]].y - py);
        if (aj > ai) { int swap = order[i]; order[i] = order[j]; order[j] = swap; }
    }
    for (int i = 0; i < enemy_count; i++) draw_enemy(g, &enemies[order[i]]);
}

static void render_weapon(tgl_t *g) {
    int kick = muzzle ? 10 : 0;
    tgl_fill(g, WIDTH / 2 - 58, VIEW_H - 70 + kick, 116, 70, 0x292b31);
    tgl_fill(g, WIDTH / 2 - 25, VIEW_H - 104 + kick, 50, 60, 0x474b55);
    tgl_fill(g, WIDTH / 2 - 9, VIEW_H - 126 + kick, 18, 45, 0x17191d);
    if (muzzle) {
        tgl_fill(g, WIDTH / 2 - 3, VIEW_H - 146, 6, 20, 0xfff08a);
        tgl_fill(g, WIDTH / 2 - 12, VIEW_H - 139, 24, 7, 0xff8b28);
    }
}

static void render_hud(tgl_t *g) {
    tgl_fill(g, 0, VIEW_H, WIDTH, HEIGHT - VIEW_H, 0x18191d);
    tgl_fill(g, 0, VIEW_H, WIDTH, 3, 0x7b251d);
    char value[16];
    text(g, 14, VIEW_H + 10, "HEALTH", 0xb4a79a, 1); number(value, health); text(g, 14, VIEW_H + 24, value, health < 30 ? 0xff3b2f : 0xf3e6d2, 2);
    text(g, 168, VIEW_H + 10, "AMMO", 0xb4a79a, 1); number(value, ammo); text(g, 168, VIEW_H + 24, value, ammo ? 0xf3e6d2 : 0xff3b2f, 2);
    text(g, 300, VIEW_H + 10, "DEMONS", 0xb4a79a, 1); number(value, kills); text(g, 300, VIEW_H + 24, value, 0xf3e6d2, 2);
    text(g, 420, VIEW_H + 10, "WASD MOVE  ARROWS TURN", 0x9a9085, 1);
    text(g, 420, VIEW_H + 25, "SPACE FIRE  Q QUIT", 0x9a9085, 1);
    if (flash) tgl_blend(g, 0, 0, WIDTH, VIEW_H, 0xff0000, flash * 20);
    if (dead || won) {
        tgl_blend(g, 0, 0, WIDTH, VIEW_H, 0x000000, 165);
        const char *title = won ? "SECTOR CLEARED" : "YOU DIED";
        text(g, WIDTH / 2 - (int)strlen(title) * 12, 128, title, won ? 0xffd45a : 0xff4035, 3);
        text(g, WIDTH / 2 - 96, 176, "PRESS R TO RESTART", 0xffffff, 1);
    }
}

static void render(tgl_t *g) {
    render_world(g); render_sprites(g); render_weapon(g); render_hud(g);
}

static void sound(int frequency, int duration, int volume) {
    if (sound_ready) taudio_tone(frequency, duration, volume);
}
static void collect_tile(void) {
    int mx = px / TILE, my = py / TILE;
    if (map_data[my][mx] == 'H') { health += 25; if (health > 100) health = 100; map_data[my][mx] = '.'; sound(660, 45, 35); }
    if (map_data[my][mx] == 'A') { ammo += 12; map_data[my][mx] = '.'; sound(440, 45, 35); }
    if (map_data[my][mx] == 'X' && kills == total_enemies) { won = 1; sound(523, 80, 45); sound(784, 120, 50); }
}
static void move_player(int amount, int strafe) {
    int direction = angle + (strafe ? 64 : 0);
    int nx = px + (int)(((int64_t)tgl_icos(direction) * amount) >> 16);
    int ny = py + (int)(((int64_t)tgl_isin(direction) * amount) >> 16);
    int radius = 42;
    if (!wall_at(nx + (nx > px ? radius : -radius), py)) px = nx;
    if (!wall_at(px, ny + (ny > py ? radius : -radius))) py = ny;
    collect_tile();
}
static void fire_weapon(void) {
    if (dead || won || ammo <= 0) { sound(90, 35, 25); return; }
    ammo--; muzzle = 3; sound(105, 45, 60);
    int best = -1, best_forward = MAX_RAY;
    for (int i = 0; i < enemy_count; i++) if (enemies[i].alive) {
        int dx = enemies[i].x - px, dy = enemies[i].y - py;
        int forward = (int)(((int64_t)dx * tgl_icos(angle) + (int64_t)dy * tgl_isin(angle)) >> 16);
        int side = (int)((-(int64_t)dx * tgl_isin(angle) + (int64_t)dy * tgl_icos(angle)) >> 16);
        if (forward > 0 && iabs(side) < forward / 7 + 24 && forward < best_forward && line_clear(px, py, enemies[i].x, enemies[i].y)) {
            best = i; best_forward = forward;
        }
    }
    if (best >= 0 && --enemies[best].hp <= 0) { enemies[best].alive = 0; kills++; sound(55, 70, 45); }
}

static void update_enemies(void) {
    for (int i = 0; i < enemy_count; i++) {
        Enemy *enemy = &enemies[i]; if (!enemy->alive) continue;
        int dx = px - enemy->x, dy = py - enemy->y;
        int distance = iabs(dx) + iabs(dy);
        if (enemy->cooldown > 0) enemy->cooldown--;
        if (distance < 180 && enemy->cooldown == 0) {
            health -= 8; flash = 5; enemy->cooldown = 35; sound(72, 45, 45);
            if (health <= 0) { health = 0; dead = 1; sound(48, 160, 55); }
        } else if (distance < 1800 && line_clear(enemy->x, enemy->y, px, py)) {
            int step_x = dx > 0 ? 5 : dx < 0 ? -5 : 0;
            int step_y = dy > 0 ? 5 : dy < 0 ? -5 : 0;
            if (!wall_at(enemy->x + step_x, enemy->y)) enemy->x += step_x;
            if (!wall_at(enemy->x, enemy->y + step_y)) enemy->y += step_y;
        }
    }
}

static int selftest(void) {
    load_level();
    if (wall_at(0, 0) == 0 || wall_at(px, py) != 0) return 1;
    if (ray_distance(px, py, angle) <= TILE / 2) return 2;
    int before = ammo; fire_weapon();
    if (ammo != before - 1) return 3;
    for (int i = 0; i < enemy_count; i++) enemies[i].alive = 0;
    kills = total_enemies; px = 19 * TILE + TILE / 2; py = TILE + TILE / 2; collect_tile();
    if (!won) return 4;
    static const char message[] = "[doom] map + raycast + combat + exit self-test OK\n";
    write(1, message, sizeof message - 1);
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--selftest")) return selftest();
    int screen_width = 1024, screen_height = 768; tgl_screen_size(&screen_width, &screen_height);
    tgl_t graphics;
    if (tgl_open(&graphics, WIDTH, HEIGHT) != 0) { write(2, "doom: no display\n", 17); return 1; }
    int origin_x = (screen_width - WIDTH) / 2, origin_y = (screen_height - HEIGHT) / 2;
    if (origin_x < 0) origin_x = 0;
    if (origin_y < 0) origin_y = 0;
    sound_ready = taudio_ready(); load_level();
    sound(110, 55, 35); sound(165, 70, 35);
    for (;;) {
        render(&graphics);
        int key = tgl_present(&graphics, origin_x, origin_y);
        if (key == 'q' || key == 'Q' || key == KEY_ESC) break;
        if (key == 'r' || key == 'R') load_level();
        if (!dead && !won) {
            if (key == KEY_LEFT) angle = (angle - 5) & 255;
            else if (key == KEY_RIGHT) angle = (angle + 5) & 255;
            else if (key == KEY_UP || key == 'w' || key == 'W') move_player(28, 0);
            else if (key == KEY_DOWN || key == 's' || key == 'S') move_player(-24, 0);
            else if (key == 'a' || key == 'A') move_player(-24, 1);
            else if (key == 'd' || key == 'D') move_player(24, 1);
            else if (key == ' ') fire_weapon();
            update_enemies();
        }
        if (flash) flash--;
        if (muzzle) muzzle--;
        usleep(16000);
    }
    taudio_stop(); tgl_finish(&graphics); return 0;
}