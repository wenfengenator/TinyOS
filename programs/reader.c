/* SPDX-License-Identifier: GPL-2.0-only */

/* Fullscreen TinyGL text and Markdown reader for TinyOS.
 *
 * Usage: reader FILE.txt|FILE.md
 * Controls: Up/Down scroll, Left/B previous page, Right/Space next page,
 * g/G first/last page, Q or Esc close.
 */
#include <tinygl.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <stdint.h>
#include <tinflate.h>

#define KEY_UP 0x80
#define KEY_DOWN 0x81
#define KEY_LEFT 0x82
#define KEY_RIGHT 0x83
#define KEY_ESC 0x1b
#define MAX_FILE (2 * 1024 * 1024)

enum { R_BODY, R_H1, R_H2, R_H3, R_H4, R_H5, R_H6, R_LIST, R_QUOTE, R_CODE, R_RULE, R_BLANK, R_TABLE, R_IMAGE };
typedef struct {
    char *text;
    uint32_t *pixels;
    int image_w, image_h, display_w, display_h;
    unsigned char style;
} Row;
typedef struct { Row *row; int count, capacity, markdown; } Document;

static int ends_with(const char *text, const char *suffix) {
    int a = (int)strlen(text), b = (int)strlen(suffix);
    return a >= b && !strcmp(text + a - b, suffix);
}
static uint32_t utf8_next(const unsigned char *text, int length, int *at) {
    unsigned c = text[(*at)++];
    if (c < 0x80) return c;
    int need = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC2 ? 1 : 0;
    uint32_t value = need == 3 ? c & 7u : need == 2 ? c & 15u : need == 1 ? c & 31u : 0xFFFDu;
    if (!need || *at + need > length) return 0xFFFDu;
    for (int i = 0; i < need; i++) {
        unsigned tail = text[(*at)++];
        if ((tail & 0xC0) != 0x80) return 0xFFFDu;
        value = value << 6 | (tail & 0x3Fu);
    }
    if ((need == 1 && value < 0x80) || (need == 2 && value < 0x800) ||
        (need == 3 && value < 0x10000) || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) return 0xFFFDu;
    return value;
}
static int append_ascii(char **out, int *used, int *capacity, const char *text) {
    int n = (int)strlen(text);
    if (*used + n + 1 > *capacity) {
        int next = *capacity;
        while (*used + n + 1 > next) next *= 2;
        char *grown = realloc(*out, (size_t)next); if (!grown) return -1;
        *out = grown; *capacity = next;
    }
    memcpy(*out + *used, text, (size_t)n); *used += n; return 0;
}
static const char *latin_ascii(uint32_t cp) {
    if ((cp >= 0x00C0 && cp <= 0x00C5) || (cp >= 0x00E0 && cp <= 0x00E5)) return "a";
    if (cp == 0x00C6 || cp == 0x00E6) return "ae";
    if (cp == 0x00C7 || cp == 0x00E7) return "c";
    if ((cp >= 0x00C8 && cp <= 0x00CB) || (cp >= 0x00E8 && cp <= 0x00EB)) return "e";
    if ((cp >= 0x00CC && cp <= 0x00CF) || (cp >= 0x00EC && cp <= 0x00EF)) return "i";
    if (cp == 0x00D0 || cp == 0x00F0) return "d";
    if (cp == 0x00D1 || cp == 0x00F1) return "n";
    if ((cp >= 0x00D2 && cp <= 0x00D6) || (cp >= 0x00F2 && cp <= 0x00F6) || cp == 0x00D8 || cp == 0x00F8) return "o";
    if ((cp >= 0x00D9 && cp <= 0x00DC) || (cp >= 0x00F9 && cp <= 0x00FC)) return "u";
    if (cp == 0x00DD || cp == 0x00FD || cp == 0x00FF) return "y";
    if (cp == 0x00DE || cp == 0x00FE) return "th";
    if (cp == 0x00DF) return "ss";
    return 0;
}
static const char *wide_ascii(uint32_t cp) {
    switch (cp) {
    case 0x00A0: case 0x2000: case 0x2001: case 0x2002: case 0x2003: case 0x2004:
    case 0x2005: case 0x2006: case 0x2007: case 0x2008: case 0x2009: case 0x200A: case 0x202F: return " ";
    case 0x00A1: return "!"; case 0x00A2: return "c"; case 0x00A3: return "GBP";
    case 0x00A5: return "YEN"; case 0x00A9: return "(c)"; case 0x00AE: return "(R)";
    case 0x00AB: return "<<"; case 0x00BB: return ">>"; case 0x00B0: return " degrees";
    case 0x00B1: return "+/-"; case 0x00B7: case 0x2022: case 0x2027: return "*";
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2212: return "-";
    case 0x2014: case 0x2015: return "--";
    case 0x2018: case 0x2019: case 0x201A: case 0x201B: case 0x2032: return "'";
    case 0x201C: case 0x201D: case 0x201E: case 0x201F: case 0x2033: return "\"";
    case 0x2026: return "..."; case 0x2039: return "<"; case 0x203A: return ">";
    case 0x2044: return "/"; case 0x20AC: return "EUR"; case 0x2122: return "(TM)";
    case 0x2190: return "<-"; case 0x2191: return "^"; case 0x2192: return "->"; case 0x2193: return "v";
    case 0x2260: return "!="; case 0x2264: return "<="; case 0x2265: return ">=";
    case 0x3000: return " "; case 0x3001: return ","; case 0x3002: return ".";
    case 0x3008: case 0x300A: case 0x300C: case 0x300E: case 0x3010: return "[";
    case 0x3009: case 0x300B: case 0x300D: case 0x300F: case 0x3011: return "]";
    default: return latin_ascii(cp);
    }
}
static char *normalize_utf8(const char *data, int length, int *out_length) {
    int capacity = length + 32, used = 0, at = 0; if (capacity < 64) capacity = 64;
    char *out = malloc((size_t)capacity); if (!out) return 0;
    while (at < length) {
        uint32_t cp = utf8_next((const unsigned char *)data, length, &at);
        char one[2] = {0, 0}, fallback[16]; const char *replacement = 0;
        if (cp < 0x80) { one[0] = (char)cp; replacement = one; }
        else if (cp >= 0xFF01 && cp <= 0xFF5E) { one[0] = (char)(cp - 0xFEE0); replacement = one; }
        else if (cp == 0xFEFF) continue;
        else if (cp >= 0x0300 && cp <= 0x036F) continue;
        else replacement = wide_ascii(cp);
        if (!replacement) {
            static const char hex[] = "0123456789ABCDEF"; int digits = cp <= 0xFFFF ? 4 : 6;
            fallback[0] = '['; fallback[1] = 'U'; fallback[2] = '+';
            for (int i = 0; i < digits; i++) fallback[3 + i] = hex[cp >> ((digits - 1 - i) * 4) & 15u];
            fallback[3 + digits] = ']'; fallback[4 + digits] = 0; replacement = fallback;
        }
        if (append_ascii(&out, &used, &capacity, replacement) != 0) { free(out); return 0; }
    }
    out[used] = 0; *out_length = used; return out;
}
static int add_row(Document *doc, const char *text, int length, int style) {
    if (doc->count == doc->capacity) {
        int next = doc->capacity ? doc->capacity * 2 : 128;
        Row *grown = realloc(doc->row, (size_t)next * sizeof *grown);
        if (!grown) return -1;
        doc->row = grown; doc->capacity = next;
    }
    char *copy = malloc((size_t)length + 1);
    if (!copy) return -1;
    memcpy(copy, text, (size_t)length); copy[length] = 0;
    memset(&doc->row[doc->count], 0, sizeof doc->row[doc->count]);
    doc->row[doc->count].text = copy;
    doc->row[doc->count++].style = (unsigned char)style;
    return 0;
}
static void free_document(Document *doc) {
    for (int i = 0; i < doc->count; i++) { free(doc->row[i].text); free(doc->row[i].pixels); }
    free(doc->row); memset(doc, 0, sizeof *doc);
}
static int entity(const char *source, int length, int *used, uint32_t *out) {
    static const struct { const char *name; uint32_t value; } named[] = {
        {"amp;", '&'}, {"lt;", '<'}, {"gt;", '>'}, {"quot;", '"'}, {"apos;", '\''}, {"nbsp;", 0x00A0},
        {"ndash;", 0x2013}, {"mdash;", 0x2014}, {"lsquo;", 0x2018}, {"rsquo;", 0x2019},
        {"ldquo;", 0x201C}, {"rdquo;", 0x201D}, {"bull;", 0x2022}, {"hellip;", 0x2026},
        {"copy;", 0x00A9}, {"reg;", 0x00AE}, {"trade;", 0x2122}, {"euro;", 0x20AC}
    };
    for (unsigned i = 0; i < sizeof named / sizeof named[0]; i++) {
        int n = (int)strlen(named[i].name);
        if (length >= n && !strncmp(source, named[i].name, (size_t)n)) { *used = n; *out = named[i].value; return 1; }
    }
    if (length > 2 && source[0] == '#') {
        int base = 10, at = 1, value = 0;
        if (source[at] == 'x' || source[at] == 'X') { base = 16; at++; }
        int start = at;
        while (at < length && source[at] != ';') {
            int digit = source[at] >= '0' && source[at] <= '9' ? source[at] - '0' :
                source[at] >= 'a' && source[at] <= 'f' ? source[at] - 'a' + 10 :
                source[at] >= 'A' && source[at] <= 'F' ? source[at] - 'A' + 10 : -1;
            if (digit < 0 || digit >= base) return 0;
            value = value * base + digit; at++;
        }
        if (at < length && at > start && value > 0 && value <= 0x10FFFF) { *used = at + 1; *out = (uint32_t)value; return 1; }
    }
    return 0;
}
static int clean_inline(const char *source, int length, char *out, int capacity) {
    int n = 0;
    for (int i = 0; i < length && n < capacity - 1; i++) {
        char c = source[i];
        if (c == '\\' && i + 1 < length) { out[n++] = source[++i]; continue; }
        if (c == '*' || c == '_' || c == '`' || (c == '~' && i + 1 < length && source[i + 1] == '~')) {
            if (c == '~') i++;
            continue;
        }
        if (c == '&') {
            int used = 0; uint32_t value = 0;
            if (entity(source + i + 1, length - i - 1, &used, &value)) {
                char one[2] = {(char)value, 0}; const char *replacement = value < 128 ? one : wide_ascii(value);
                if (replacement) for (int j = 0; replacement[j] && n < capacity - 1; j++) out[n++] = replacement[j];
                i += used; continue;
            }
        }
        if (c == '<') {
            int close = i + 1; while (close < length && source[close] != '>') close++;
            if (close < length) {
                int html = source[i + 1] == '/' || source[i + 1] == '!' ||
                    (source[i + 1] >= 'A' && source[i + 1] <= 'Z') || (source[i + 1] >= 'a' && source[i + 1] <= 'z');
                if (html && !memchr(source + i + 1, ':', (size_t)(close - i - 1))) { i = close; continue; }
                i++; while (i < close && n < capacity - 1) out[n++] = source[i++]; i = close; continue;
            }
        }
        if (c == '!' && i + 1 < length && source[i + 1] == '[') c = 0;
        if (c == '[') {
            int close = i + 1; while (close < length && source[close] != ']') close++;
            if (close < length) { i++; while (i < close && n < capacity - 1) out[n++] = source[i++]; i = close; continue; }
        }
        if (c == ']' && i + 1 < length && source[i + 1] == '(') {
            i += 2; while (i < length && source[i] != ')') i++; continue;
        }
        if (c) out[n++] = c;
    }
    while (n > 0 && out[n - 1] == ' ') n--;
    out[n] = 0; return n;
}
static int image_target(const char *line, int length, char *path, int capacity, char *alt, int alt_capacity) {
    int at = 0; while (at < length && (line[at] == ' ' || line[at] == '\t')) at++;
    if (at + 4 < length && line[at] == '!' && line[at + 1] == '[') {
        int ac = at + 2; while (ac < length && line[ac] != ']') ac++;
        if (ac + 2 >= length || line[ac + 1] != '(') return 0;
        int pe = ac + 2; while (pe < length && line[pe] != ')' && line[pe] != ' ' && line[pe] != '\t') pe++;
        int pn = pe - ac - 2, an = ac - at - 2;
        if (pn <= 0 || pn >= capacity) return 0;
        memcpy(path, line + ac + 2, (size_t)pn); path[pn] = 0;
        if (an >= alt_capacity) an = alt_capacity - 1;
        memcpy(alt, line + at + 2, (size_t)an); alt[an] = 0;
        return 1;
    }
    if (at + 5 < length && !strncmp(line + at, "<img ", 5)) {
        const char *end = memchr(line + at, '>', (size_t)(length - at));
        const char *src = strstr(line + at, "src=");
        if (!end || !src || src >= end) return 0;
        src += 4; if (*src != '\'' && *src != '"') return 0;
        char quote = *src++; const char *pe = src; while (pe < end && *pe != quote) pe++;
        int pn = (int)(pe - src); if (pn <= 0 || pn >= capacity) return 0;
        memcpy(path, src, (size_t)pn); path[pn] = 0;
        strncpy(alt, "image", (size_t)alt_capacity); alt[alt_capacity - 1] = 0;
        return 1;
    }
    return 0;
}
static int line_is(const char *line, int length, char marker) {
    int count = 0;
    for (int i = 0; i < length; i++) {
        if (line[i] == marker) count++;
        else if (line[i] != ' ' && line[i] != '\t') return 0;
    }
    return count >= 3;
}
static int wrap_line(Document *doc, const char *text, int length, int style, int columns) {
    if (length <= 0) return add_row(doc, "", 0, R_BLANK);
    int prefix = style == R_LIST || style == R_QUOTE ? 2 : 0;
    int width = columns - prefix; if (width < 12) width = 12;
    int start = 0, first = 1;
    while (start < length) {
        while (start < length && text[start] == ' ') start++;
        int end = start + width;
        if (end >= length) end = length;
        else {
            int split = end;
            while (split > start && text[split] != ' ') split--;
            if (split > start) end = split;
        }
        char line[1024]; int n = 0;
        if (prefix) { line[n++] = first ? (style == R_LIST ? '-' : '>') : ' '; line[n++] = ' '; }
        int take = end - start;
        if (take > (int)sizeof line - n - 1) take = (int)sizeof line - n - 1;
        memcpy(line + n, text + start, (size_t)take); n += take; line[n] = 0;
        if (add_row(doc, line, n, style) != 0) return -1;
        first = 0; start = end; while (start < length && text[start] == ' ') start++;
    }
    return 0;
}
static int parse_document(Document *doc, const char *data, int length, int columns, int markdown) {
    memset(doc, 0, sizeof *doc); doc->markdown = markdown;
    int at = 0, in_code = 0;
    while (at <= length) {
        int end = at; while (end < length && data[end] != '\n') end++;
        int n = end - at; if (n && data[at + n - 1] == '\r') n--;
        const char *line = data + at; int style = R_BODY, skip = 0;
        if (markdown) {
            if (n >= 3 && (!strncmp(line, "```", 3) || !strncmp(line, "~~~", 3))) { in_code = !in_code; at = end + 1; if (end == length) break; continue; }
            if (in_code) style = R_CODE;
            else if (n >= 4 && !strncmp(line, "    ", 4)) { style = R_CODE; skip = 4; }
            else if (line[0] == '#') {
                while (skip < n && skip < 6 && line[skip] == '#') skip++;
                if (skip < n && line[skip] == ' ') { style = R_H1 + skip - 1; skip++; }
                else skip = 0;
            }
            else if (n > 0 && line[0] == '>') { style = R_QUOTE; while (skip < n && (line[skip] == '>' || line[skip] == ' ')) skip++; }
            else if (n > 1 && (line[0] == '-' || line[0] == '*' || line[0] == '+') && line[1] == ' ') { style = R_LIST; skip = 2; }
            else {
                int digit = 0;
                while (digit < n && line[digit] >= '0' && line[digit] <= '9') digit++;
                if (digit > 0 && digit + 1 < n && (line[digit] == '.' || line[digit] == ')') && line[digit + 1] == ' ') {
                    style = R_LIST; skip = digit + 2;
                }
            }
            if (style == R_BODY && (line_is(line, n, '=') || line_is(line, n, '-')) && doc->count > 0 && doc->row[doc->count - 1].style == R_BODY) {
                int heading = line_is(line, n, '=') ? R_H1 : R_H2;
                for (int i = doc->count - 1; i >= 0 && doc->row[i].style == R_BODY; i--) doc->row[i].style = (unsigned char)heading;
                at = end + 1; if (end == length) break; continue;
            }
            if (style == R_BODY && (line_is(line, n, '-') || line_is(line, n, '*') || line_is(line, n, '_'))) style = R_RULE;
            if (style == R_BODY && n > 2 && memchr(line, '|', (size_t)n)) style = R_TABLE;
            char image_path[512], image_alt[256];
            if (style == R_BODY && image_target(line, n, image_path, sizeof image_path, image_alt, sizeof image_alt)) {
                if (add_row(doc, image_path, (int)strlen(image_path), R_IMAGE) != 0) return -1;
                at = end + 1; if (end == length) break; continue;
            }
        }
        if (style == R_RULE) { if (add_row(doc, "", 0, style) != 0) return -1; }
        else if (style == R_CODE) { if (wrap_line(doc, line + skip, n - skip, style, columns - 4) != 0) return -1; }
        else {
            char clean[2048]; int clean_n = markdown ? clean_inline(line + skip, n - skip, clean, sizeof clean) : n;
            const char *shown = markdown ? clean : line;
            int width = columns;
            if (style == R_H1) width = columns * 2 / 3;
            else if (style >= R_H2 && style <= R_H6) width = columns * 4 / 5;
            else if (style == R_TABLE) width = columns - 2;
            if (wrap_line(doc, shown, clean_n, style, width) != 0) return -1;
        }
        if (end == length) break;
        at = end + 1;
    }
    return 0;
}
static char *read_file(const char *path, int *length) {
    int fd = open(path, O_RDONLY); if (fd < 0) return 0;
    struct stat st; int capacity = 4096;
    if (fstat(fd, &st) == 0 && st.st_size > 0 && st.st_size <= MAX_FILE) capacity = (int)st.st_size + 1;
    char *data = malloc((size_t)capacity + 1); if (!data) { close(fd); return 0; }
    int used = 0;
    for (;;) {
        if (used == capacity) {
            if (capacity >= MAX_FILE) { free(data); close(fd); return 0; }
            int next = capacity * 2; if (next > MAX_FILE) next = MAX_FILE;
            char *grown = realloc(data, (size_t)next + 1); if (!grown) { free(data); close(fd); return 0; }
            data = grown; capacity = next;
        }
        int got = (int)read(fd, data + used, (size_t)(capacity - used));
        if (got < 0) { free(data); close(fd); return 0; }
        if (!got) break;
        used += got;
    }
    close(fd); data[used] = 0; *length = used; return data;
}
static uint16_t le16(const unsigned char *p) { return (uint16_t)(p[0] | (uint16_t)p[1] << 8); }
static uint32_t le32(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t be32(const unsigned char *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static int image_alloc(Row *row, int width, int height) {
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096 || (size_t)width * height > 16777216u) return -1;
    row->pixels = malloc((size_t)width * height * sizeof *row->pixels);
    if (!row->pixels) return -1;
    row->image_w = width; row->image_h = height;
    return 0;
}
static int decode_bmp(Row *row, const unsigned char *data, int length) {
    if (length < 54 || data[0] != 'B' || data[1] != 'M') return -1;
    uint32_t offset = le32(data + 10), dib = le32(data + 14);
    int width = (int)le32(data + 18), signed_h = (int)le32(data + 22);
    int height = signed_h < 0 ? -signed_h : signed_h;
    int bits = le16(data + 28);
    if (dib < 40 || le16(data + 26) != 1 || le32(data + 30) != 0 || (bits != 24 && bits != 32)) return -1;
    size_t stride = ((size_t)width * bits + 31u) / 32u * 4u;
    if (image_alloc(row, width, height) != 0 || offset > (uint32_t)length || stride * (size_t)height > (size_t)length - offset) return -1;
    for (int y = 0; y < height; y++) {
        int source_y = signed_h > 0 ? height - 1 - y : y;
        const unsigned char *source = data + offset + stride * (size_t)source_y;
        for (int x = 0; x < width; x++) {
            unsigned b = source[x * (bits / 8)], g = source[x * (bits / 8) + 1], r = source[x * (bits / 8) + 2];
            unsigned a = bits == 32 ? source[x * 4 + 3] : 255;
            row->pixels[y * width + x] = a << 24 | r << 16 | g << 8 | b;
        }
    }
    return 0;
}
static int paeth(int left, int up, int upper_left) {
    int prediction = left + up - upper_left, dl = prediction - left, du = prediction - up, dul = prediction - upper_left;
    if (dl < 0) dl = -dl;
    if (du < 0) du = -du;
    if (dul < 0) dul = -dul;
    return dl <= du && dl <= dul ? left : du <= dul ? up : upper_left;
}
static int decode_png(Row *row, const unsigned char *data, int length) {
    static const unsigned char signature[8] = {137,80,78,71,13,10,26,10};
    if (length < 33 || memcmp(data, signature, 8)) return -1;
    int width = 0, height = 0, depth = 0, color = 0, interlace = 0, channels = 0;
    uint32_t palette[256]; int palette_n = 0;
    unsigned char alpha[256]; memset(alpha, 255, sizeof alpha);
    unsigned char *compressed = 0; size_t compressed_n = 0;
    for (int at = 8; at + 12 <= length;) {
        uint32_t size = be32(data + at); const unsigned char *type = data + at + 4, *chunk = data + at + 8;
        if (size > (uint32_t)(length - at - 12)) { free(compressed); return -1; }
        if (!memcmp(type, "IHDR", 4) && size == 13) {
            width = (int)be32(chunk); height = (int)be32(chunk + 4); depth = chunk[8]; color = chunk[9]; interlace = chunk[12];
        } else if (!memcmp(type, "PLTE", 4) && size % 3 == 0 && size <= 768) {
            palette_n = (int)(size / 3);
            for (int i = 0; i < palette_n; i++) palette[i] = 0xFF000000u | (uint32_t)chunk[i * 3] << 16 | (uint32_t)chunk[i * 3 + 1] << 8 | chunk[i * 3 + 2];
        } else if (!memcmp(type, "tRNS", 4) && size <= 256) memcpy(alpha, chunk, size);
        else if (!memcmp(type, "IDAT", 4)) {
            if (compressed_n + size > 16u * 1024u * 1024u) { free(compressed); return -1; }
            unsigned char *grown = realloc(compressed, compressed_n + size);
            if (!grown) { free(compressed); return -1; }
            compressed = grown; memcpy(compressed + compressed_n, chunk, size); compressed_n += size;
        } else if (!memcmp(type, "IEND", 4)) break;
        at += 12 + (int)size;
    }
    if (depth != 8 || interlace || !compressed_n) { free(compressed); return -1; }
    channels = color == 0 ? 1 : color == 2 ? 3 : color == 3 ? 1 : color == 4 ? 2 : color == 6 ? 4 : 0;
    if (!channels || (color == 3 && !palette_n) || width <= 0 || height <= 0 || width > 4096 || height > 4096) { free(compressed); return -1; }
    size_t rowbytes = (size_t)width * channels, raw_n = (rowbytes + 1) * (size_t)height;
    if (raw_n > 64u * 1024u * 1024u) { free(compressed); return -1; }
    unsigned char *raw = malloc(raw_n); if (!raw) { free(compressed); return -1; }
    long inflated = tinfl_zlib(compressed, compressed_n, raw, raw_n, 0); free(compressed);
    if (inflated != (long)raw_n || image_alloc(row, width, height) != 0) { free(raw); return -1; }
    for (int y = 0; y < height; y++) {
        unsigned char filter = raw[(rowbytes + 1) * (size_t)y];
        unsigned char *scan = raw + (rowbytes + 1) * (size_t)y + 1;
        unsigned char *previous = y ? raw + (rowbytes + 1) * (size_t)(y - 1) + 1 : 0;
        if (filter > 4) { free(raw); return -1; }
        for (size_t x = 0; x < rowbytes; x++) {
            int left = x >= (size_t)channels ? scan[x - channels] : 0, up = previous ? previous[x] : 0;
            int upper_left = previous && x >= (size_t)channels ? previous[x - channels] : 0;
            if (filter == 1) scan[x] = (unsigned char)(scan[x] + left);
            else if (filter == 2) scan[x] = (unsigned char)(scan[x] + up);
            else if (filter == 3) scan[x] = (unsigned char)(scan[x] + (left + up) / 2);
            else if (filter == 4) scan[x] = (unsigned char)(scan[x] + paeth(left, up, upper_left));
        }
        for (int x = 0; x < width; x++) {
            unsigned r, g, b, a = 255; const unsigned char *pixel = scan + x * channels;
            if (color == 0 || color == 4) r = g = b = pixel[0];
            else if (color == 3) { unsigned index = pixel[0]; if (index >= (unsigned)palette_n) index = 0; r = palette[index] >> 16 & 255; g = palette[index] >> 8 & 255; b = palette[index] & 255; a = alpha[index]; }
            else { r = pixel[0]; g = pixel[1]; b = pixel[2]; }
            if (color == 4) a = pixel[1]; else if (color == 6) a = pixel[3];
            row->pixels[y * width + x] = a << 24 | r << 16 | g << 8 | b;
        }
    }
    free(raw); return 0;
}
static int ppm_token(const unsigned char *data, int length, int *at, int *value) {
    while (*at < length) {
        if (data[*at] == '#') while (*at < length && data[(*at)++] != '\n') {}
        else if (data[*at] == ' ' || data[*at] == '\t' || data[*at] == '\r' || data[*at] == '\n') (*at)++;
        else break;
    }
    int start = *at, number = 0;
    while (*at < length && data[*at] >= '0' && data[*at] <= '9') number = number * 10 + data[(*at)++] - '0';
    if (*at == start) return -1;
    *value = number;
    return 0;
}
static int decode_ppm(Row *row, const unsigned char *data, int length) {
    if (length < 3 || data[0] != 'P' || (data[1] != '3' && data[1] != '6')) return -1;
    int ascii = data[1] == '3';
    int at = 2, width, height, maximum;
    if (ppm_token(data, length, &at, &width) || ppm_token(data, length, &at, &height) || ppm_token(data, length, &at, &maximum) || maximum <= 0 || maximum > 65535) return -1;
    if (image_alloc(row, width, height) != 0) return -1;
    if (ascii) {
        for (int i = 0; i < width * height; i++) {
            int r, g, b;
            if (ppm_token(data, length, &at, &r) || ppm_token(data, length, &at, &g) || ppm_token(data, length, &at, &b)) return -1;
            if (r > maximum || g > maximum || b > maximum) return -1;
            r = r * 255 / maximum; g = g * 255 / maximum; b = b * 255 / maximum;
            row->pixels[i] = 0xFF000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
        }
    } else {
        while (at < length && (data[at] == ' ' || data[at] == '\t' || data[at] == '\r' || data[at] == '\n')) at++;
        if (maximum != 255 || (size_t)width * height * 3 > (size_t)(length - at)) return -1;
        for (int i = 0; i < width * height; i++) row->pixels[i] = 0xFF000000u | (uint32_t)data[at + i * 3] << 16 | (uint32_t)data[at + i * 3 + 1] << 8 | data[at + i * 3 + 2];
    }
    return 0;
}
static int decode_image(Row *row, const unsigned char *data, int length) {
    if (decode_png(row, data, length) == 0) return 0;
    free(row->pixels); row->pixels = 0;
    if (decode_bmp(row, data, length) == 0) return 0;
    free(row->pixels); row->pixels = 0;
    if (decode_ppm(row, data, length) == 0) return 0;
    free(row->pixels); row->pixels = 0; row->image_w = row->image_h = 0;
    return -1;
}
static void load_document_images(Document *doc, const char *document_path, int max_width, int max_height) {
    char directory[512] = "."; const char *slash = strrchr(document_path, '/');
    if (slash) { int n = (int)(slash - document_path); if (n >= (int)sizeof directory) n = sizeof directory - 1; memcpy(directory, document_path, (size_t)n); directory[n] = 0; }
    for (int i = 0; i < doc->count; i++) if (doc->row[i].style == R_IMAGE) {
        Row *row = &doc->row[i]; char full[1024];
        if (strstr(row->text, "://") || !strncmp(row->text, "data:", 5)) { row->display_h = 34; continue; }
        if (row->text[0] == '/') snprintf(full, sizeof full, "%s", row->text);
        else snprintf(full, sizeof full, "%s/%s", directory, row->text);
        int length = 0; char *data = read_file(full, &length);
        if (!data || decode_image(row, (unsigned char *)data, length) != 0) { free(data); row->display_h = 34; continue; }
        free(data); row->display_w = row->image_w; row->display_h = row->image_h;
        if (row->display_w > max_width) { row->display_h = row->display_h * max_width / row->display_w; row->display_w = max_width; }
        if (row->display_h > max_height) { row->display_w = row->display_w * max_height / row->display_h; row->display_h = max_height; }
        if (row->display_w < 1) row->display_w = 1;
        if (row->display_h < 1) row->display_h = 1;
    }
}
static void text_scaled(tgl_t *graphics, int x, int y, const char *text, uint32_t color, int scale) {
    for (; *text; text++, x += 8 * scale) {
        unsigned ch = (unsigned char)*text; if (ch < 0x20 || ch > 0x7e) continue;
        const unsigned char *glyph = tgl_font8x8[ch - 0x20];
        for (int row = 0; row < 8; row++) for (int bit = 0; bit < 8; bit++)
            if (glyph[row] & (1u << bit)) tgl_fill(graphics, x + bit * scale, y + row * scale, scale, scale, color);
    }
}
static void draw_image(tgl_t *graphics, const Row *row, int x, int y) {
    if (!row->pixels || row->display_w <= 0 || row->display_h <= 0) return;
    for (int dy = 0; dy < row->display_h; dy++) {
        int sy = dy * row->image_h / row->display_h;
        for (int dx = 0; dx < row->display_w; dx++) {
            int sx = dx * row->image_w / row->display_w;
            uint32_t pixel = row->pixels[sy * row->image_w + sx];
            uint32_t rgb = pixel & 0xFFFFFFu; int alpha_value = (int)(pixel >> 24);
            if (alpha_value == 255) tgl_pixel(graphics, x + dx, y + dy, rgb);
            else if (alpha_value) tgl_blend(graphics, x + dx, y + dy, 1, 1, rgb, alpha_value);
        }
    }
}
static int row_height(const Row *row, int body_scale) {
    int style = row->style;
    if (style == R_H1) return body_scale * 14;
    if (style == R_H2) return body_scale * 12;
    if (style == R_H3) return body_scale * 11;
    if (style >= R_H4 && style <= R_H6) return body_scale * 10;
    if (style == R_BLANK) return body_scale * 7;
    if (style == R_RULE) return body_scale * 8;
    if (style == R_IMAGE) return row->display_h + body_scale * 12;
    return body_scale * 10;
}
static int page_end(const Document *doc, int top, int available, int body_scale) {
    int used = 0, at = top;
    while (at < doc->count) {
        int height = row_height(&doc->row[at], body_scale);
        if (used + height > available && at > top) break;
        used += height; at++;
    }
    return at;
}
static int previous_page(const Document *doc, int top, int available, int body_scale) {
    int start = top, used = 0;
    while (start > 0) {
        int height = row_height(&doc->row[start - 1], body_scale);
        if (used + height > available) break;
        used += height; start--;
    }
    return start;
}
static void number_text(char *out, int value) {
    char reverse[16]; int n = 0, p = 0;
    if (!value) reverse[n++] = '0';
    while (value) { reverse[n++] = (char)('0' + value % 10); value /= 10; }
    while (n) out[p++] = reverse[--n];
    out[p] = 0;
}
static void render(tgl_t *graphics, const Document *doc, int top, const char *name, int body_scale) {
    int margin = graphics->w / 14; if (margin < 28) margin = 28;
    int header = 54, footer = 38, content_h = graphics->h - header - footer;
    tgl_clear(graphics, 0xF3F0E8);
    tgl_fill(graphics, 0, 0, graphics->w, header, 0x203A43);
    tgl_fill(graphics, 0, graphics->h - footer, graphics->w, footer, 0xD9D4C8);
    text_scaled(graphics, margin, 16, name, 0xF8F3E7, 2);
    int y = header + 14, end = page_end(doc, top, content_h - 24, body_scale);
    for (int i = top; i < end; i++) {
        const Row *row = &doc->row[i]; int style = row->style, scale = body_scale, x = margin;
        uint32_t color = 0x263238;
        if (style == R_H1) { scale = body_scale + 1; color = 0x183E52; }
        else if (style == R_H2) { scale = body_scale + 1; color = 0x315D69; }
        else if (style >= R_H3 && style <= R_H6) color = 0x3A6670;
        else if (style == R_LIST) { color = 0x263238; x += body_scale * 8; }
        else if (style == R_QUOTE) { color = 0x586C72; x += body_scale * 12; tgl_fill(graphics, margin, y, 4, row_height(row, body_scale) - 2, 0x8AA6A8); }
        else if (style == R_CODE) { color = 0xDDE8E8; x += 12; tgl_fill(graphics, margin, y - 3, graphics->w - 2 * margin, row_height(row, body_scale), 0x26373A); }
        else if (style == R_TABLE) { color = 0x29434A; tgl_fill(graphics, margin, y - 2, graphics->w - 2 * margin, row_height(row, body_scale), i & 1 ? 0xE7E2D8 : 0xDED9CE); }
        else if (style == R_RULE) { tgl_fill(graphics, margin, y + 5, graphics->w - 2 * margin, 2, 0xA9A49A); y += row_height(row, body_scale); continue; }
        else if (style == R_IMAGE) {
            if (row->pixels) { x = (graphics->w - row->display_w) / 2; draw_image(graphics, row, x, y); y += row->display_h + 4; }
            else { tgl_fill(graphics, margin, y, graphics->w - 2 * margin, 28, 0xDED9CE); text_scaled(graphics, margin + 8, y + 6, "IMAGE UNAVAILABLE: ", 0x8A4B3A, 1); text_scaled(graphics, margin + 160, y + 6, row->text, 0x5F625F, 1); y += 34; }
            continue;
        }
        text_scaled(graphics, x, y, row->text, color, scale);
        y += row_height(row, body_scale);
    }
    char status[64], a[16], b[16]; number_text(a, doc->count ? top + 1 : 0); number_text(b, doc->count);
    int n = 0; const char *prefix = "ROWS "; while (*prefix) status[n++] = *prefix++;
    for (int i = 0; a[i]; i++) status[n++] = a[i];
    status[n++] = '/';
    for (int i = 0; b[i]; i++) status[n++] = b[i];
    status[n] = 0;
    text_scaled(graphics, margin, graphics->h - footer + 11, "UP/DOWN SCROLL  LEFT/B PREV  RIGHT/SPACE NEXT  g/G ENDS  Q CLOSE", 0x40545A, 1);
    text_scaled(graphics, graphics->w - margin - (int)strlen(status) * 8, graphics->h - footer + 11, status, 0x40545A, 1);
}
static int selftest(void) {
    static const char sample[] = "# Reader\n###### Details\n\nSetext\n---\nA **clear** and ~~current~~ paragraph with [links](https://example.com), <em>HTML</em> &amp; entities.\nentity&mdash;dash &#8212; numeric\n\n- [x] task\n1. first item\n> quoted text\n| A | B |\n|---|---|\n| 1 | 2 |\n![pixel](pixel.bmp)\n```\nint value = 7;\n```\n";
    Document doc; if (parse_document(&doc, sample, sizeof sample - 1, 42, 1) != 0) return 1;
    int headings = 0, code = 0, list = 0, table = 0, h6 = 0, image = -1;
    for (int i = 0; i < doc.count; i++) { headings += doc.row[i].style == R_H1 || doc.row[i].style == R_H2; h6 += doc.row[i].style == R_H6; code += doc.row[i].style == R_CODE; list += doc.row[i].style == R_LIST; table += doc.row[i].style == R_TABLE; if (doc.row[i].style == R_IMAGE) image = i; }
    int entity_ok = 0; for (int i = 0; i < doc.count; i++) entity_ok += strstr(doc.row[i].text, "entity--dash -- numeric") != 0;
    if (headings < 2 || !h6 || !code || list < 2 || !table || image < 0 || !entity_ok) { free_document(&doc); return 2; }
    unsigned char bmp[70]; memset(bmp, 0, sizeof bmp);
    bmp[0] = 'B'; bmp[1] = 'M'; bmp[2] = sizeof bmp; bmp[10] = 54; bmp[14] = 40; bmp[18] = 2; bmp[22] = 2; bmp[26] = 1; bmp[28] = 24;
    bmp[54] = 255; bmp[57] = 255; bmp[62] = 255; bmp[65] = 255; bmp[66] = 255;
    if (decode_image(&doc.row[image], bmp, sizeof bmp) != 0) { free_document(&doc); return 3; }
    doc.row[image].display_w = 40; doc.row[image].display_h = 40;
    static const unsigned char png[] = {
        137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,2,0,0,0,2,8,2,0,0,0,253,212,154,115,
        0,0,0,21,73,68,65,84,120,156,99,248,207,192,192,240,159,129,17,72,252,255,207,0,0,30,246,4,253,
        134,93,91,169,0,0,0,0,73,69,78,68,174,66,96,130
    };
    Row png_row; memset(&png_row, 0, sizeof png_row);
    if (decode_image(&png_row, png, sizeof png) != 0 || png_row.image_w != 2 || png_row.image_h != 2 || png_row.pixels[3] != 0xFFFFFFFFu) {
        free(png_row.pixels); free_document(&doc); return 4;
    }
    free(png_row.pixels);
    static const unsigned char ppm[] = "P3\n# source-controlled image\n2 1\n255\n255 0 0  0 128 255\n";
    memset(&png_row, 0, sizeof png_row);
    if (decode_image(&png_row, ppm, sizeof ppm - 1) != 0 || png_row.pixels[0] != 0xFFFF0000u || png_row.pixels[1] != 0xFF0080FFu) {
        free(png_row.pixels); free_document(&doc); return 5;
    }
    free(png_row.pixels);
    int next = page_end(&doc, 0, 60, 2);
    if (next <= 0 || previous_page(&doc, next, 60, 2) != 0) { free_document(&doc); return 6; }
    tgl_t graphics; memset(&graphics, 0, sizeof graphics);
    graphics.w = graphics.stride = 800; graphics.h = 600; graphics.rsh = 16; graphics.gsh = 8;
    graphics.px = calloc((size_t)graphics.w * graphics.h, sizeof *graphics.px); if (!graphics.px) { free_document(&doc); return 6; }
    render(&graphics, &doc, 0, "sample.md", 2);
    int painted = 0; for (int i = 0; i < graphics.w * graphics.h; i++) painted += graphics.px[i] != 0;
    free(graphics.px); free_document(&doc);
    if (painted <= graphics.w * graphics.h / 2) return 7;
    static const char plain[] = "literal **markers** and # signs\nsecond line\n";
    if (parse_document(&doc, plain, sizeof plain - 1, 80, 0) != 0) return 8;
    int ok = doc.count >= 2 && strstr(doc.row[0].text, "**markers**") != 0;
    free_document(&doc);
    if (!ok) return 9;
    static const char utf8[] = "Wide \xE2\x80\x94 dash \xE2\x80\x93 quotes \xE2\x80\x9Cyes\xE2\x80\x9D\xE2\x80\xA6 caf\xC3\xA9 \xEF\xBC\xA1 \xE4\xB8\xAD";
    int normalized_n = 0; char *normalized = normalize_utf8(utf8, sizeof utf8 - 1, &normalized_n);
    static const char expected[] = "Wide -- dash - quotes \"yes\"... cafe A [U+4E2D]";
    if (!normalized || normalized_n != (int)sizeof expected - 1 || strcmp(normalized, expected)) { free(normalized); return 10; }
    free(normalized); return 0;
}
static int check_file(const char *path) {
    int length = 0; char *data = read_file(path, &length);
    if (!data) return 1;
    int normalized_length = 0; char *normalized = normalize_utf8(data, length, &normalized_length); free(data);
    if (!normalized) return 2;
    Document doc; int markdown = ends_with(path, ".md") || ends_with(path, ".markdown");
    if (parse_document(&doc, normalized, normalized_length, 64, markdown) != 0) { free(normalized); return 2; }
    free(normalized); load_document_images(&doc, path, 800, 500);
    int failed = doc.count == 0;
    for (int i = 0; i < doc.count; i++) if (doc.row[i].style == R_IMAGE && !doc.row[i].pixels) failed = 1;
    free_document(&doc); return failed ? 3 : 0;
}
int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--selftest")) return selftest();
    if (argc == 3 && !strcmp(argv[1], "--check")) return check_file(argv[2]);
    if (argc != 2) { write(2, "usage: reader FILE.txt|FILE.md\n", 31); return 1; }
    int length = 0; char *data = read_file(argv[1], &length);
    if (!data) { write(2, "reader: cannot read file\n", 25); return 1; }
    int normalized_length = 0; char *normalized = normalize_utf8(data, length, &normalized_length); free(data);
    if (!normalized) { write(2, "reader: document too large\n", 27); return 1; }
    int screen_w = 1024, screen_h = 768; tgl_screen_size(&screen_w, &screen_h);
    int body_scale = 2;
    int margin = screen_w / 14; if (margin < 28) margin = 28;
    int columns = (screen_w - 2 * margin) / (8 * body_scale); if (columns < 20) columns = 20;
    Document doc; int markdown = ends_with(argv[1], ".md") || ends_with(argv[1], ".markdown");
    if (parse_document(&doc, normalized, normalized_length, columns, markdown) != 0) { free(normalized); write(2, "reader: document too large\n", 27); return 1; }
    free(normalized);
    load_document_images(&doc, argv[1], screen_w - 2 * margin, screen_h - 150);
    tgl_t graphics;
    if (tgl_open(&graphics, screen_w, screen_h) != 0) { free_document(&doc); write(2, "reader: no display\n", 19); return 1; }
    int title_length = 0; char *title = normalize_utf8(argv[1], (int)strlen(argv[1]), &title_length);
    if (!title) title = argv[1];
    int top = 0, content_h = screen_h - 54 - 38 - 24;
    for (;;) {
        render(&graphics, &doc, top, title, body_scale);
        int key = tgl_present(&graphics, 0, 0);
        if (key == 'q' || key == 'Q' || key == KEY_ESC) break;
        if (key == KEY_DOWN && top + 1 < doc.count) top++;
        else if (key == KEY_UP && top > 0) top--;
        else if (key == KEY_RIGHT || key == ' ' || key == 'n' || key == 'N') { int next = page_end(&doc, top, content_h, body_scale); if (next < doc.count) top = next; }
        else if (key == KEY_LEFT || key == 'b' || key == 'B') top = previous_page(&doc, top, content_h, body_scale);
        else if (key == 'g') top = 0;
        else if (key == 'G') { top = doc.count; top = previous_page(&doc, top, content_h, body_scale); }
        usleep(16000);
    }
    if (title != argv[1]) free(title);
    tgl_finish(&graphics); free_document(&doc); return 0;
}
