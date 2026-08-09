/* SPDX-License-Identifier: GPL-2.0-only */

/* mbb — a musl-static multi-call "mini-busybox" for TinyOS.
 *
 * Proves the Tier-1 Unix substrate: a shell that runs pipelines (fork + pipe +
 * dup2 + wait4) plus a broad set of coreutils-style applets.  Invoked multi-call
 * ("mbb APPLET args" / "/MBB.BIN sh"), by applet name (argv[0], symlink-style),
 * or with no args (a built-in `echo hello | grep hello` self-test).
 *
 * Only applets the current kernel syscalls support are included; things needing
 * networking, real permissions, symlinks or rename are intentionally left out. */
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <dirent.h>
#include <time.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <regex.h>
#include "tinygl.h"        /* TinyGL client library (the UMD under test) */
#include "tnet.h"          /* kernel DNS resolver + ICMP echo helpers */
#include "tglob.h"         /* pathname expansion for shell arguments */

extern char **environ;

static void outs(int fd, const char *s) { write(fd, s, strlen(s)); }
static void out1(const char *s) { outs(1, s); }
static void outnum(int fd, long v) {
    char t[24]; int i = 0, neg = v < 0; unsigned long u = neg ? -(unsigned long)v : (unsigned long)v;
    if (!u) t[i++] = '0'; else while (u) { t[i++] = (char)('0' + u % 10); u /= 10; }
    char b[26]; int n = 0; if (neg) b[n++] = '-'; while (i) b[n++] = t[--i];
    write(fd, b, n);
}
static int contains(const char *h, const char *n) {
    if (!*n) return 1;
    for (; *h; h++) { const char *a = h, *b = n; while (*a && *b && *a == *b) { a++; b++; } if (!*b) return 1; }
    return 0;
}
static int ci_eq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) if ((*a | 32) != (*b | 32)) return 0;
    return *a == *b;
}

/* ---- filters ------------------------------------------------------------- */
static int ap_echo(int argc, char **argv) {
    int i = 1, nl = 1;
    if (i < argc && !strcmp(argv[i], "-n")) { nl = 0; i++; }
    for (; i < argc; i++) { outs(1, argv[i]); if (i + 1 < argc) write(1, " ", 1); }
    if (nl) write(1, "\n", 1);
    return 0;
}
static int catfd(int fd) { char b[512]; int n; while ((n = read(fd, b, sizeof b)) > 0) if (write(1, b, n) != n) return 1; return 0; }
/* input fd for a filter: open the file arg at argv[i], else stdin (0); -1 on error */
static int in_fd(int argc, char **argv, int i, const char *name) {
    if (i >= argc) return 0;
    int fd = open(argv[i], O_RDONLY);
    if (fd < 0) { outs(2, name); outs(2, ": "); outs(2, argv[i]); outs(2, ": not found\n"); }
    return fd;
}
static int ap_cat(int argc, char **argv) {
    if (argc < 2) return catfd(0);
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) { outs(2, "cat: "); outs(2, argv[i]); outs(2, ": not found\n"); rc = 1; continue; }
        if (catfd(fd)) rc = 1;
        close(fd);
    }
    return rc;
}
/* pager prompt: one raw key from /dev/tty (space=page, Enter=line, q=quit) */
static int more_key(int tty) {
    struct termios old, raw;
    tcgetattr(1, &old); raw = old; raw.c_lflag &= ~(ICANON | ECHO); tcsetattr(1, TCSANOW, &raw);
    out1("\x1b[7m--More--\x1b[0m");
    char c = ' '; if (read(tty, &c, 1) <= 0) c = 'q';
    tcsetattr(1, TCSANOW, &old);
    out1("\r\x1b[K");                              /* erase the prompt */
    return (unsigned char)c;
}
/* more [file...] — page long output a screenful at a time.  Keys come from
 * /dev/tty so pagination works even when the content is piped (`ls | more`). */
static int ap_more(int argc, char **argv) {
    struct winsize ws; int rows = 24;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 1) rows = ws.ws_row;
    int tty = open("/dev/tty", O_RDONLY);
    int nf = 0; for (int i = 1; i < argc; i++) if (argv[i][0] != '-') nf++;
    int quit = 0, rc = 0, shown = 0;
    for (int idx = 0; !quit; idx++) {
        int fd;
        if (nf) {
            int fi = -1, seen = 0;
            for (int i = 1; i < argc; i++) if (argv[i][0] != '-' && seen++ == idx) { fi = i; break; }
            if (fi < 0) break;
            fd = open(argv[fi], O_RDONLY);
            if (fd < 0) { outs(2, "more: "); outs(2, argv[fi]); outs(2, ": not found\n"); rc = 1; continue; }
        } else { if (idx > 0) break; fd = 0; }
        char b[512]; int n;
        while (!quit && (n = read(fd, b, sizeof b)) > 0) {
            for (int j = 0; j < n; j++) {
                write(1, &b[j], 1);
                if (b[j] != '\n') continue;
                if (++shown < rows - 1) continue;
                if (tty < 0) { shown = 0; continue; }         /* no key source: keep scrolling */
                int k = more_key(tty);
                if (k == 'q' || k == 'Q') { quit = 1; break; }
                shown = (k == '\n' || k == '\r') ? rows - 2 : 0;   /* Enter=one line, else a page */
            }
        }
        if (fd != 0) close(fd);
    }
    if (tty >= 0) close(tty);
    return rc;
}
static int grep_fixed(const char *line, const char *pat, int icase) {
    if (!icase) return contains(line, pat);
    if (!*pat) return 1;
    for (; *line; line++) {
        const char *a = line, *b = pat;
        while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; }
        if (!*b) return 1;
    }
    return 0;
}
static long read_line_fd(int fd, char **line, size_t *capacity) {
    size_t length = 0;
    for (;;) {
        char ch; int got = read(fd, &ch, 1);
        if (got <= 0) { if (!length) return -1; break; }
        if (length + 2 > *capacity) {
            size_t next = *capacity ? *capacity * 2 : 256;
            char *grown = realloc(*line, next);
            if (!grown) return -2;
            *line = grown; *capacity = next;
        }
        (*line)[length++] = ch;
        if (ch == '\n') break;
    }
    (*line)[length] = 0;
    return (long)length;
}
static int grep_file(const char *name, regex_t *regex, const char *pat, int fixed,
                     int icase, int invert, int number, int count_only,
                     int list, int quiet, int prefix, int *error) {
    int fd = name ? open(name, O_RDONLY) : 0;
    if (fd < 0) { outs(2, "grep: "); outs(2, name); outs(2, ": not found\n"); *error = 1; return 0; }
    char *line = 0; size_t capacity = 0; long length, line_no = 0, matches = 0;
    while ((length = read_line_fd(fd, &line, &capacity)) >= 0) {
        line_no++;
        int newline = length > 0 && line[length - 1] == '\n';
        if (newline) line[length - 1] = 0;
        int matched = fixed ? grep_fixed(line, pat, icase) : regexec(regex, line, 0, 0, 0) == 0;
        if (matched == invert) continue;
        matches++;
        if (quiet || list) break;
        if (!count_only) {
            if (prefix) { out1(name); write(1, ":", 1); }
            if (number) { outnum(1, line_no); write(1, ":", 1); }
            write(1, line, newline ? length - 1 : length); write(1, "\n", 1);
        }
    }
    if (length == -2) *error = 1;
    free(line); if (fd) close(fd);
    if (count_only) { if (prefix) { out1(name); write(1, ":", 1); } outnum(1, matches); write(1, "\n", 1); }
    if (list && matches) { out1(name ? name : "(standard input)"); write(1, "\n", 1); }
    return matches != 0;
}
static int ap_grep(int argc, char **argv) {
    int invert = 0, icase = 0, number = 0, fixed = 0, count_only = 0, list = 0, quiet = 0, i = 1;
    const char *pat = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        if (!strcmp(argv[i], "-e")) { if (++i >= argc) break; pat = argv[i]; continue; }
        for (const char *flag = argv[i] + 1; *flag; flag++) {
            if (*flag == 'v') invert = 1; else if (*flag == 'i') icase = 1;
            else if (*flag == 'n') number = 1; else if (*flag == 'F') fixed = 1;
            else if (*flag == 'c') count_only = 1; else if (*flag == 'l') list = 1;
            else if (*flag == 'q') quiet = 1; else if (*flag != 'E') {
                outs(2, "usage: grep [-EFinvclq] [-e pattern] PATTERN [FILE...]\n"); return 2;
            }
        }
    }
    if (!pat && i < argc) pat = argv[i++];
    if (!pat) { outs(2, "usage: grep [-EFinvclq] [-e pattern] PATTERN [FILE...]\n"); return 2; }
    regex_t regex; int compiled = 0;
    if (!fixed) {
        int code = regcomp(&regex, pat, REG_EXTENDED | REG_NOSUB | (icase ? REG_ICASE : 0));
        if (code) { char message[160]; regerror(code, &regex, message, sizeof message); outs(2, "grep: "); outs(2, message); outs(2, "\n"); return 2; }
        compiled = 1;
    }
    int found = 0, error = 0, files = argc - i;
    if (!files) found = grep_file(0, &regex, pat, fixed, icase, invert, number, count_only, list, quiet, 0, &error);
    else for (; i < argc; i++) {
        int matched = grep_file(argv[i], &regex, pat, fixed, icase, invert, number, count_only, list, quiet, files > 1 && !list, &error);
        if (matched) found = 1;
        if (quiet && found) break;
    }
    if (compiled) regfree(&regex);
    return error ? 2 : (found ? 0 : 1);
}

#define SED_MAX_CMD 32
struct sed_cmd {
    int address, line, op, global, print, address_compiled, regex_compiled;
    regex_t address_regex, regex;
    char *replacement;
};
static int sed_delimited(const char **cursor, char delimiter, char **result) {
    size_t capacity = 64, length = 0; char *text = malloc(capacity);
    if (!text) return -1;
    while (**cursor && **cursor != delimiter) {
        char ch = *(*cursor)++;
        if (ch == '\\' && **cursor) {
            char next = **cursor;
            if (next == delimiter) { ch = next; (*cursor)++; }
            else { if (length + 1 >= capacity) { capacity *= 2; text = realloc(text, capacity); if (!text) return -1; } text[length++] = ch; ch = *(*cursor)++; }
        }
        if (length + 1 >= capacity) { capacity *= 2; text = realloc(text, capacity); if (!text) return -1; }
        text[length++] = ch;
    }
    if (**cursor != delimiter) { free(text); return -1; }
    (*cursor)++; text[length] = 0; *result = text; return 0;
}
static void sed_free(struct sed_cmd *commands, int count) {
    for (int i = 0; i < count; i++) {
        if (commands[i].address_compiled) regfree(&commands[i].address_regex);
        if (commands[i].regex_compiled) regfree(&commands[i].regex);
        if (commands[i].op == 's') free(commands[i].replacement);
    }
}
static int sed_compile(const char *script, struct sed_cmd *commands, int *count, int extended) {
    const char *cursor = script; *count = 0;
    while (*cursor) {
        while (*cursor == ' ' || *cursor == '\t' || *cursor == ';') cursor++;
        if (!*cursor) break;
        if (*count >= SED_MAX_CMD) { outs(2, "sed: too many commands\n"); return -1; }
        struct sed_cmd *command = &commands[(*count)++]; memset(command, 0, sizeof *command);
        if (isdigit((unsigned char)*cursor)) { command->address = 1; command->line = 0; while (isdigit((unsigned char)*cursor)) command->line = command->line * 10 + *cursor++ - '0'; }
        else if (*cursor == '$') { command->address = 2; cursor++; }
        else if (*cursor == '/') {
            char *pattern; cursor++;
            if (sed_delimited(&cursor, '/', &pattern) != 0) goto syntax;
            command->address = 3;
            int code = regcomp(&command->address_regex, pattern, extended ? REG_EXTENDED | REG_NOSUB : REG_NOSUB);
            free(pattern); if (code) goto regex_error; command->address_compiled = 1;
        }
        while (*cursor == ' ' || *cursor == '\t') cursor++;
        command->op = *cursor++;
        if (command->op == 's') {
            char delimiter = *cursor++, *pattern = 0;
            if (!delimiter || delimiter == '\n' || sed_delimited(&cursor, delimiter, &pattern) != 0 ||
                sed_delimited(&cursor, delimiter, &command->replacement) != 0) { free(pattern); goto syntax; }
            int code = regcomp(&command->regex, pattern, extended ? REG_EXTENDED : 0);
            free(pattern); if (code) goto regex_error; command->regex_compiled = 1;
            while (*cursor && *cursor != ';') {
                if (*cursor == 'g') command->global = 1;
                else if (*cursor == 'p') command->print = 1;
                else if (*cursor != ' ' && *cursor != '\t') goto syntax;
                cursor++;
            }
        } else if (command->op != 'p' && command->op != 'd' && command->op != 'q') goto syntax;
        while (*cursor == ' ' || *cursor == '\t') cursor++;
        if (*cursor && *cursor != ';') goto syntax;
        continue;
regex_error:
        outs(2, "sed: invalid regular expression\n"); sed_free(commands, *count); return -1;
syntax:
        outs(2, "sed: unsupported or malformed command\n"); sed_free(commands, *count); return -1;
    }
    return 0;
}
static int sed_append(char **output, size_t *length, size_t *capacity, const char *text, size_t amount) {
    if (*length + amount + 1 > *capacity) {
        size_t next = *capacity ? *capacity : 128;
        while (next < *length + amount + 1) next *= 2;
        char *grown = realloc(*output, next); if (!grown) return -1;
        *output = grown; *capacity = next;
    }
    memcpy(*output + *length, text, amount); *length += amount; (*output)[*length] = 0; return 0;
}
static char *sed_substitute(const char *line, struct sed_cmd *command, int *changed) {
    char *output = 0; size_t length = 0, capacity = 0, offset = 0, line_length = strlen(line);
    regmatch_t match[10]; *changed = 0;
    do {
        int code = regexec(&command->regex, line + offset, 10, match, offset ? REG_NOTBOL : 0);
        if (code || match[0].rm_so < 0) { if (sed_append(&output, &length, &capacity, line + offset, line_length - offset) != 0) goto fail; break; }
        size_t start = offset + (size_t)match[0].rm_so, end = offset + (size_t)match[0].rm_eo;
        if (sed_append(&output, &length, &capacity, line + offset, start - offset) != 0) goto fail;
        for (const char *replacement = command->replacement; *replacement; replacement++) {
            if (*replacement == '&') { if (sed_append(&output, &length, &capacity, line + start, end - start) != 0) goto fail; }
            else if (*replacement == '\\' && replacement[1] >= '1' && replacement[1] <= '9') {
                int group = *++replacement - '0';
                if (match[group].rm_so >= 0 && sed_append(&output, &length, &capacity,
                    line + offset + match[group].rm_so, (size_t)(match[group].rm_eo - match[group].rm_so)) != 0) goto fail;
            } else if (sed_append(&output, &length, &capacity, replacement, 1) != 0) goto fail;
        }
        *changed = 1;
        if (!command->global) { if (sed_append(&output, &length, &capacity, line + end, line_length - end) != 0) goto fail; break; }
        if (end == offset) {
            if (end >= line_length) break;
            if (sed_append(&output, &length, &capacity, line + end, 1) != 0) goto fail;
            offset = end + 1;
        } else offset = end;
    } while (offset <= line_length);
    if (!output) output = strdup(line);
    return output;
fail:
    free(output); return 0;
}
static int sed_address(struct sed_cmd *command, const char *line, long number, int last) {
    if (!command->address) return 1;
    if (command->address == 1) return number == command->line;
    if (command->address == 2) return last;
    return regexec(&command->address_regex, line, 0, 0, 0) == 0;
}
static int sed_read_all(int argc, char **argv, int first, char ***lines, int *count) {
    int capacity = 0; *lines = 0; *count = 0;
    int inputs = argc - first; if (!inputs) inputs = 1;
    for (int input = 0; input < inputs; input++) {
        int fd = argc > first ? open(argv[first + input], O_RDONLY) : 0;
        if (fd < 0) { outs(2, "sed: "); outs(2, argv[first + input]); outs(2, ": not found\n"); return -1; }
        char *line = 0; size_t line_capacity = 0; long length;
        while ((length = read_line_fd(fd, &line, &line_capacity)) >= 0) {
            if (length && line[length - 1] == '\n') line[--length] = 0;
            if (*count == capacity) { capacity = capacity ? capacity * 2 : 64; *lines = realloc(*lines, (size_t)capacity * sizeof **lines); if (!*lines) return -1; }
            (*lines)[(*count)++] = strdup(line);
        }
        free(line); if (fd) close(fd);
    }
    return 0;
}
static int ap_sed(int argc, char **argv) {
    int quiet = 0, extended = 0, i = 1; char *script = 0; size_t script_length = 0, script_capacity = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "-n")) quiet = 1;
        else if (!strcmp(argv[i], "-E") || !strcmp(argv[i], "-r")) extended = 1;
        else if (!strcmp(argv[i], "-e") && i + 1 < argc) {
            const char *part = argv[++i];
            if (script_length && sed_append(&script, &script_length, &script_capacity, ";", 1) != 0) { free(script); return 2; }
            if (sed_append(&script, &script_length, &script_capacity, part, strlen(part)) != 0) { free(script); return 2; }
        }
        else { outs(2, "usage: sed [-nE] SCRIPT [FILE...]\n"); return 2; }
    }
    if (!script && i < argc) script = strdup(argv[i++]);
    if (!script) { outs(2, "usage: sed [-nE] [-e SCRIPT] [FILE...]\n"); return 2; }
    struct sed_cmd commands[SED_MAX_CMD]; int command_count;
    if (sed_compile(script, commands, &command_count, extended) != 0) { free(script); return 2; }
    free(script);
    char **lines; int line_count;
    if (sed_read_all(argc, argv, i, &lines, &line_count) != 0) { sed_free(commands, command_count); return 2; }
    int quit = 0;
    for (int index = 0; index < line_count && !quit; index++) {
        char *line = lines[index]; int deleted = 0;
        for (int command_index = 0; command_index < command_count; command_index++) {
            struct sed_cmd *command = &commands[command_index];
            if (!sed_address(command, line, index + 1, index == line_count - 1)) continue;
            if (command->op == 's') {
                int changed; char *next = sed_substitute(line, command, &changed);
                if (!next) { deleted = 1; break; }
                free(line); line = next;
                if (changed && command->print) { out1(line); write(1, "\n", 1); }
            } else if (command->op == 'p') { out1(line); write(1, "\n", 1); }
            else if (command->op == 'd') { deleted = 1; break; }
            else if (command->op == 'q') { quit = 1; break; }
        }
        if (!quiet && !deleted) { out1(line); write(1, "\n", 1); }
        free(line);
        for (int rest = index + 1; quit && rest < line_count; rest++) { free(lines[rest]); lines[rest] = 0; }
    }
    free(lines); sed_free(commands, command_count); return 0;
}

#define AWK_MAX_VAR 64
#define AWK_MAX_FIELD 128
#define AWK_MAX_CLAUSE 32
struct awk_var { char name[32]; char *value; };
struct awk_ctx {
    struct awk_var vars[AWK_MAX_VAR]; int variable_count;
    char *record, *work, *fields[AWK_MAX_FIELD]; int field_count;
    long nr, fnr; const char *filename; char fs[32], ofs[32];
};
struct awk_value { char text[512]; double number; int numeric; };
struct awk_parser { const char *cursor; struct awk_ctx *ctx; int error; };
struct awk_clause { int type; char *pattern, *action; };
static struct awk_value awk_expression(struct awk_parser *parser);
static void awk_skip(struct awk_parser *parser) { while (*parser->cursor == ' ' || *parser->cursor == '\t') parser->cursor++; }
static struct awk_value awk_string(const char *text) {
    struct awk_value value; snprintf(value.text, sizeof value.text, "%s", text ? text : "");
    char *end; value.number = strtod(value.text, &end); value.numeric = end != value.text && !*end; return value;
}
static struct awk_value awk_number(double number) {
    struct awk_value value; value.number = number; value.numeric = 1;
    snprintf(value.text, sizeof value.text, "%.6g", number); return value;
}
static const char *awk_get(struct awk_ctx *ctx, const char *name) {
    for (int i = 0; i < ctx->variable_count; i++) if (!strcmp(ctx->vars[i].name, name)) return ctx->vars[i].value;
    return "";
}
static int awk_set(struct awk_ctx *ctx, const char *name, const char *value) {
    if (!strcmp(name, "FS")) { snprintf(ctx->fs, sizeof ctx->fs, "%s", value); return 0; }
    if (!strcmp(name, "OFS")) { snprintf(ctx->ofs, sizeof ctx->ofs, "%s", value); return 0; }
    for (int i = 0; i < ctx->variable_count; i++) if (!strcmp(ctx->vars[i].name, name)) {
        char *copy = strdup(value); if (!copy) return -1; free(ctx->vars[i].value); ctx->vars[i].value = copy; return 0;
    }
    if (ctx->variable_count >= AWK_MAX_VAR || strlen(name) >= sizeof ctx->vars[0].name) return -1;
    struct awk_var *variable = &ctx->vars[ctx->variable_count++]; strcpy(variable->name, name);
    variable->value = strdup(value); return variable->value ? 0 : -1;
}
static struct awk_value awk_factor(struct awk_parser *parser);
static struct awk_value awk_product(struct awk_parser *parser) {
    struct awk_value value = awk_factor(parser); awk_skip(parser);
    while (*parser->cursor == '*' || *parser->cursor == '/') {
        char op = *parser->cursor++; struct awk_value right = awk_factor(parser);
        double divisor = right.number; value = awk_number(op == '*' ? value.number * divisor : (divisor ? value.number / divisor : 0)); awk_skip(parser);
    }
    return value;
}
static struct awk_value awk_sum(struct awk_parser *parser) {
    struct awk_value value = awk_product(parser); awk_skip(parser);
    while (*parser->cursor == '+' || *parser->cursor == '-') {
        char op = *parser->cursor++; struct awk_value right = awk_product(parser);
        value = awk_number(op == '+' ? value.number + right.number : value.number - right.number); awk_skip(parser);
    }
    return value;
}
static struct awk_value awk_factor(struct awk_parser *parser) {
    awk_skip(parser);
    if (*parser->cursor == '(') { parser->cursor++; struct awk_value value = awk_sum(parser); awk_skip(parser); if (*parser->cursor == ')') parser->cursor++; else parser->error = 1; return value; }
    if (*parser->cursor == '-') { parser->cursor++; struct awk_value value = awk_factor(parser); return awk_number(-value.number); }
    if (*parser->cursor == '"') {
        parser->cursor++; char text[512]; int length = 0;
        while (*parser->cursor && *parser->cursor != '"') {
            char ch = *parser->cursor++;
            if (ch == '\\' && *parser->cursor) { ch = *parser->cursor++; if (ch == 'n') ch = '\n'; else if (ch == 't') ch = '\t'; }
            if (length < (int)sizeof text - 1) text[length++] = ch;
        }
        if (*parser->cursor == '"') parser->cursor++; else parser->error = 1;
        text[length] = 0; return awk_string(text);
    }
    if (*parser->cursor == '$') {
        parser->cursor++; int field = 0;
        if (isdigit((unsigned char)*parser->cursor)) while (isdigit((unsigned char)*parser->cursor)) field = field * 10 + *parser->cursor++ - '0';
        else { char name[32]; int length = 0; while (isalnum((unsigned char)*parser->cursor) || *parser->cursor == '_') if (length < 31) name[length++] = *parser->cursor++; else parser->cursor++; name[length] = 0; field = (int)strtol(awk_get(parser->ctx, name), 0, 10); }
        if (!field) return awk_string(parser->ctx->record);
        return awk_string(field <= parser->ctx->field_count ? parser->ctx->fields[field - 1] : "");
    }
    if (isdigit((unsigned char)*parser->cursor) || *parser->cursor == '.') {
        char *end; double number = strtod(parser->cursor, &end);
        if (end == parser->cursor) { parser->error = 1; return awk_string(""); }
        parser->cursor = end; return awk_number(number);
    }
    char name[32]; int length = 0;
    while (isalnum((unsigned char)*parser->cursor) || *parser->cursor == '_') {
        if (length < 31) name[length++] = *parser->cursor;
        parser->cursor++;
    }
    name[length] = 0;
    if (!length) { parser->error = 1; return awk_string(""); }
    if (!strcmp(name, "length")) {
        awk_skip(parser);
        if (*parser->cursor != '(') return awk_number(strlen(parser->ctx->record));
        parser->cursor++; awk_skip(parser);
        struct awk_value argument = *parser->cursor == ')' ? awk_string(parser->ctx->record) : awk_expression(parser);
        awk_skip(parser); if (*parser->cursor == ')') parser->cursor++; else parser->error = 1;
        return awk_number(strlen(argument.text));
    }
    if (!strcmp(name, "NR")) return awk_number(parser->ctx->nr);
    if (!strcmp(name, "FNR")) return awk_number(parser->ctx->fnr);
    if (!strcmp(name, "NF")) return awk_number(parser->ctx->field_count);
    if (!strcmp(name, "FILENAME")) return awk_string(parser->ctx->filename ? parser->ctx->filename : "");
    return awk_string(awk_get(parser->ctx, name));
}
static int awk_regex(const char *text, const char *pattern) {
    regex_t regex; if (regcomp(&regex, pattern, REG_EXTENDED | REG_NOSUB) != 0) return -1;
    int matched = regexec(&regex, text, 0, 0, 0) == 0; regfree(&regex); return matched;
}
static struct awk_value awk_comparison(struct awk_parser *parser) {
    struct awk_value left = awk_sum(parser); awk_skip(parser);
    const char *operator = 0; int length = 0;
    if (!strncmp(parser->cursor, "!~", 2) || !strncmp(parser->cursor, "==", 2) || !strncmp(parser->cursor, "!=", 2) ||
        !strncmp(parser->cursor, ">=", 2) || !strncmp(parser->cursor, "<=", 2)) { operator = parser->cursor; length = 2; }
    else if (*parser->cursor == '~' || *parser->cursor == '>' || *parser->cursor == '<') { operator = parser->cursor; length = 1; }
    if (!operator) return left;
    char op[3] = { 0 }; memcpy(op, operator, (size_t)length); parser->cursor += length; awk_skip(parser);
    if (!strcmp(op, "~") || !strcmp(op, "!~")) {
        char pattern[512]; int pattern_length = 0;
        if (*parser->cursor == '/') { parser->cursor++; while (*parser->cursor && *parser->cursor != '/') { if (pattern_length < 255) pattern[pattern_length++] = *parser->cursor; parser->cursor++; } if (*parser->cursor == '/') parser->cursor++; else parser->error = 1; }
        else { struct awk_value right = awk_sum(parser); memcpy(pattern, right.text, sizeof pattern - 1); pattern[sizeof pattern - 1] = 0; pattern_length = strlen(pattern); }
        pattern[pattern_length] = 0; int matched = awk_regex(left.text, pattern); if (matched < 0) parser->error = 1;
        return awk_number((!strcmp(op, "!~")) ? !matched : matched);
    }
    struct awk_value right = awk_sum(parser); int comparison;
    if (left.numeric && right.numeric) comparison = left.number < right.number ? -1 : left.number > right.number;
    else comparison = strcmp(left.text, right.text);
    if (!strcmp(op, "==")) return awk_number(comparison == 0);
    if (!strcmp(op, "!=")) return awk_number(comparison != 0);
    if (!strcmp(op, ">=")) return awk_number(comparison >= 0);
    if (!strcmp(op, "<=")) return awk_number(comparison <= 0);
    if (!strcmp(op, ">")) return awk_number(comparison > 0);
    return awk_number(comparison < 0);
}
static int awk_truth(struct awk_value value) { return value.numeric ? value.number != 0 : value.text[0] != 0; }
static struct awk_value awk_expression(struct awk_parser *parser) {
    struct awk_value value = awk_comparison(parser); awk_skip(parser);
    while (!strncmp(parser->cursor, "&&", 2) || !strncmp(parser->cursor, "||", 2)) {
        int is_and = parser->cursor[0] == '&'; parser->cursor += 2;
        struct awk_value right = awk_comparison(parser);
        value = awk_number(is_and ? (awk_truth(value) && awk_truth(right)) : (awk_truth(value) || awk_truth(right)));
        awk_skip(parser);
    }
    return value;
}
static void awk_fields(struct awk_ctx *ctx, char *line) {
    free(ctx->work); ctx->work = strdup(line); ctx->field_count = 0;
    char *cursor = ctx->work;
    if (!ctx->fs[0] || !strcmp(ctx->fs, " ")) {
        while (*cursor && ctx->field_count < AWK_MAX_FIELD) {
            while (isspace((unsigned char)*cursor)) cursor++;
            if (!*cursor) break;
            ctx->fields[ctx->field_count++] = cursor;
            while (*cursor && !isspace((unsigned char)*cursor)) cursor++;
            if (*cursor) *cursor++ = 0;
        }
    } else {
        char separator = ctx->fs[0];
        for (;;) {
            if (ctx->field_count >= AWK_MAX_FIELD) break;
            ctx->fields[ctx->field_count++] = cursor; char *end = strchr(cursor, separator);
            if (!end) break;
            *end = 0; cursor = end + 1;
        }
    }
}
static char *awk_trim_copy(const char *start, const char *end) {
    while (start < end && isspace((unsigned char)*start)) start++;
    while (end > start && isspace((unsigned char)end[-1])) end--;
    size_t length = (size_t)(end - start); char *copy = malloc(length + 1); if (!copy) return 0;
    memcpy(copy, start, length); copy[length] = 0; return copy;
}
static int awk_compile(const char *program, struct awk_clause *clauses, int *count) {
    const char *cursor = program; *count = 0;
    while (*cursor) {
        while (isspace((unsigned char)*cursor) || *cursor == ';') cursor++;
        if (!*cursor) break;
        if (*count >= AWK_MAX_CLAUSE) return -1;
        const char *pattern_start = cursor, *brace = 0; int quote = 0, slash = 0;
        for (; *cursor; cursor++) {
            if (*cursor == '"' && (cursor == program || cursor[-1] != '\\') && !slash) quote = !quote;
            else if (*cursor == '/' && !quote && (cursor == program || cursor[-1] != '\\')) slash = !slash;
            else if (*cursor == '{' && !quote && !slash) { brace = cursor; break; }
        }
        struct awk_clause *clause = &clauses[(*count)++]; memset(clause, 0, sizeof *clause);
        if (!brace) { clause->pattern = awk_trim_copy(pattern_start, cursor); clause->action = strdup("print"); break; }
        clause->pattern = awk_trim_copy(pattern_start, brace); cursor = brace + 1;
        const char *action_start = cursor; quote = 0;
        while (*cursor) { if (*cursor == '"' && (cursor == action_start || cursor[-1] != '\\')) quote = !quote; else if (*cursor == '}' && !quote) break; cursor++; }
        if (*cursor != '}') return -1;
        clause->action = awk_trim_copy(action_start, cursor++);
        if (!strcmp(clause->pattern, "BEGIN")) clause->type = 1;
        else if (!strcmp(clause->pattern, "END")) clause->type = 2;
        if (!clause->pattern || !clause->action) return -1;
    }
    return 0;
}
static void awk_free_clauses(struct awk_clause *clauses, int count) { for (int i = 0; i < count; i++) { free(clauses[i].pattern); free(clauses[i].action); } }
static int awk_pattern(struct awk_ctx *ctx, const char *pattern) {
    if (!*pattern) return 1;
    size_t length = strlen(pattern);
    if (length >= 2 && pattern[0] == '/' && pattern[length - 1] == '/') {
        char *copy = strndup(pattern + 1, length - 2); if (!copy) return -1;
        int matched = awk_regex(ctx->record, copy); free(copy); return matched;
    }
    struct awk_parser parser = { pattern, ctx, 0 }; struct awk_value value = awk_expression(&parser); awk_skip(&parser);
    return parser.error || *parser.cursor ? -1 : awk_truth(value);
}
static int awk_statement(struct awk_ctx *ctx, const char *statement) {
    while (isspace((unsigned char)*statement)) statement++;
    if (!strcmp(statement, "next")) return 1;
    if (!strcmp(statement, "exit")) return 2;
    if (!strncmp(statement, "print", 5) && (!statement[5] || isspace((unsigned char)statement[5]))) {
        const char *cursor = statement + 5; while (isspace((unsigned char)*cursor)) cursor++;
        if (!*cursor) { out1(ctx->record); write(1, "\n", 1); return 0; }
        int first = 1;
        while (*cursor) {
            struct awk_parser parser = { cursor, ctx, 0 }; struct awk_value value = awk_expression(&parser);
            if (parser.error) return -1;
            if (!first) out1(ctx->ofs);
            out1(value.text); first = 0;
            cursor = parser.cursor; awk_skip(&parser); cursor = parser.cursor;
            if (*cursor == ',') { cursor++; while (isspace((unsigned char)*cursor)) cursor++; }
            else if (*cursor) return -1;
        }
        write(1, "\n", 1); return 0;
    }
    if (!strncmp(statement, "printf", 6) && (!statement[6] || isspace((unsigned char)statement[6]))) {
        const char *cursor = statement + 6; while (isspace((unsigned char)*cursor)) cursor++;
        struct awk_parser parser = { cursor, ctx, 0 }; struct awk_value format = awk_expression(&parser);
        if (parser.error) return -1; cursor = parser.cursor; while (isspace((unsigned char)*cursor)) cursor++;
        struct awk_value arguments[16]; int argument_count = 0;
        while (*cursor == ',' && argument_count < 16) {
            cursor++; while (isspace((unsigned char)*cursor)) cursor++;
            parser.cursor = cursor; parser.error = 0; arguments[argument_count++] = awk_expression(&parser);
            if (parser.error) return -1; cursor = parser.cursor; while (isspace((unsigned char)*cursor)) cursor++;
        }
        if (*cursor) return -1;
        int argument = 0;
        for (const char *ch = format.text; *ch; ch++) {
            if (*ch != '%') { write(1, ch, 1); continue; }
            if (ch[1] == '%') { write(1, "%", 1); ch++; continue; }
            const char *start = ch++; while (*ch && strchr("-+ #0.123456789", *ch)) ch++;
            if (!*ch || argument >= argument_count || !strchr("sdigf", *ch)) return -1;
            char spec[32]; size_t spec_length = (size_t)(ch - start + 1); if (spec_length >= sizeof spec) return -1;
            memcpy(spec, start, spec_length); spec[spec_length] = 0; char output[512];
            if (*ch == 's') snprintf(output, sizeof output, spec, arguments[argument].text);
            else if (*ch == 'd' || *ch == 'i') snprintf(output, sizeof output, spec, (int)arguments[argument].number);
            else snprintf(output, sizeof output, spec, arguments[argument].number);
            out1(output); argument++;
        }
        return 0;
    }
    const char *cursor = statement; char name[32]; int name_length = 0;
    if (!(isalpha((unsigned char)*cursor) || *cursor == '_')) return -1;
    while (isalnum((unsigned char)*cursor) || *cursor == '_') { if (name_length < 31) name[name_length++] = *cursor; cursor++; }
    name[name_length] = 0; while (isspace((unsigned char)*cursor)) cursor++;
    if (!strcmp(cursor, "++")) { struct awk_value old = awk_string(awk_get(ctx, name)), value = awk_number(old.number + 1); return awk_set(ctx, name, value.text); }
    char op = 0;
    if (*cursor == '=' && cursor[1] != '=') { op = '='; cursor++; }
    else if ((*cursor == '+' || *cursor == '-' || *cursor == '*' || *cursor == '/') && cursor[1] == '=') { op = *cursor; cursor += 2; }
    else return -1;
    struct awk_parser parser = { cursor, ctx, 0 }; struct awk_value value = awk_expression(&parser); awk_skip(&parser);
    if (parser.error || *parser.cursor) return -1;
    if (op != '=') { struct awk_value old = awk_string(awk_get(ctx, name)); double right = value.number;
        value = awk_number(op == '+' ? old.number + right : op == '-' ? old.number - right : op == '*' ? old.number * right : (right ? old.number / right : 0)); }
    return awk_set(ctx, name, value.text);
}
static int awk_action(struct awk_ctx *ctx, const char *action) {
    const char *start = action, *cursor = action; int quote = 0;
    for (;;) {
        if (*cursor == '"' && (cursor == action || cursor[-1] != '\\')) quote = !quote;
        if ((!*cursor || (*cursor == ';' && !quote))) {
            char *statement = awk_trim_copy(start, cursor); if (!statement) return -1;
            int result = *statement ? awk_statement(ctx, statement) : 0; free(statement);
            if (result) return result;
            if (!*cursor) break;
            start = cursor + 1;
        }
        cursor++;
    }
    return 0;
}
static int awk_run_clauses(struct awk_ctx *ctx, struct awk_clause *clauses, int count, int type) {
    for (int i = 0; i < count; i++) if (clauses[i].type == type) {
        int matches = type ? 1 : awk_pattern(ctx, clauses[i].pattern);
        if (matches < 0) return -1;
        if (matches) { int result = awk_action(ctx, clauses[i].action); if (result) return result; }
    }
    return 0;
}
static int ap_awk(int argc, char **argv) {
    struct awk_ctx ctx; memset(&ctx, 0, sizeof ctx); strcpy(ctx.fs, " "); strcpy(ctx.ofs, " ");
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strncmp(argv[i], "-F", 2)) { const char *fs = argv[i] + 2; if (!*fs && ++i < argc) fs = argv[i]; if (!*fs) return 2; snprintf(ctx.fs, sizeof ctx.fs, "%s", fs); }
        else if (!strcmp(argv[i], "-v") && ++i < argc) { char *equal = strchr(argv[i], '='); if (!equal) return 2; *equal = 0; awk_set(&ctx, argv[i], equal + 1); *equal = '='; }
        else { outs(2, "usage: awk [-F sep] [-v name=value] PROGRAM [FILE...]\n"); return 2; }
    }
    if (i >= argc) { outs(2, "usage: awk [-F sep] [-v name=value] PROGRAM [FILE...]\n"); return 2; }
    struct awk_clause clauses[AWK_MAX_CLAUSE]; int clause_count;
    if (awk_compile(argv[i++], clauses, &clause_count) != 0) { outs(2, "awk: malformed program\n"); return 2; }
    int files[128], file_count = 0, status = 0;
    for (int argument = i; argument < argc; argument++) {
        char *equal = strchr(argv[argument], '='); int valid = equal && equal != argv[argument];
        for (char *ch = argv[argument]; valid && ch < equal; ch++) if (!(isalnum((unsigned char)*ch) || *ch == '_') || (ch == argv[argument] && isdigit((unsigned char)*ch))) valid = 0;
        if (valid) { *equal = 0; if (awk_set(&ctx, argv[argument], equal + 1) != 0) { *equal = '='; status = -1; break; } *equal = '='; }
        else if (file_count < 128) files[file_count++] = argument;
    }
    ctx.record = (char *)"";
    if (status >= 0) status = awk_run_clauses(&ctx, clauses, clause_count, 1);
    int stop = status == 2;
    int inputs = file_count ? file_count : 1;
    for (int input = 0; input < inputs && !stop && status >= 0; input++) {
        const char *name = file_count ? argv[files[input]] : 0; int fd = name ? open(name, O_RDONLY) : 0;
        if (fd < 0) { outs(2, "awk: "); outs(2, name); outs(2, ": not found\n"); status = -1; break; }
        ctx.filename = name ? name : "-"; ctx.fnr = 0; char *line = 0; size_t capacity = 0; long length;
        while (!stop && (length = read_line_fd(fd, &line, &capacity)) >= 0) {
            if (length && line[length - 1] == '\n') line[--length] = 0;
            ctx.record = line; ctx.nr++; ctx.fnr++; awk_fields(&ctx, line);
            int result = awk_run_clauses(&ctx, clauses, clause_count, 0);
            if (result < 0) { status = -1; break; } if (result == 2) stop = 1;
        }
        free(line); if (fd) close(fd);
    }
    if (status >= 0) { ctx.record = (char *)""; int result = awk_run_clauses(&ctx, clauses, clause_count, 2); if (result < 0) status = -1; }
    if (status < 0) outs(2, "awk: unsupported or malformed expression\n");
    free(ctx.work); for (int variable = 0; variable < ctx.variable_count; variable++) free(ctx.vars[variable].value);
    awk_free_clauses(clauses, clause_count); return status < 0 ? 2 : 0;
}
static int ap_wc(int argc, char **argv) {
    int wl = 0, ww = 0, wc = 0, any = 0, i = 1;
    for (; i < argc && argv[i][0] == '-'; i++)
        for (const char *f = argv[i] + 1; *f; f++) { if (*f == 'l') wl = 1; else if (*f == 'w') ww = 1; else if (*f == 'c') wc = 1; any = 1; }
    if (!any) wl = ww = wc = 1;
    int fd = in_fd(argc, argv, i, "wc"); if (fd < 0) return 1;
    long lines = 0, words = 0, chars = 0; int inw = 0, r; char b[512];
    while ((r = read(fd, b, sizeof b)) > 0) for (int k = 0; k < r; k++) {
        char c = b[k]; chars++; if (c == '\n') lines++;
        if (c == ' ' || c == '\n' || c == '\t') inw = 0; else if (!inw) { inw = 1; words++; }
    }
    if (fd) close(fd);
    if (wl) { outnum(1, lines); write(1, " ", 1); }
    if (ww) { outnum(1, words); write(1, " ", 1); }
    if (wc) { outnum(1, chars); write(1, " ", 1); }
    write(1, "\n", 1);
    return 0;
}
static int ap_head(int argc, char **argv) {
    int n = 10, i = 1;
    if (i < argc && !strcmp(argv[i], "-n") && i + 1 < argc) { n = atoi(argv[i + 1]); i += 2; }
    int fd = in_fd(argc, argv, i, "head"); if (fd < 0) return 1;
    long out = 0; char c;
    while (out < n && read(fd, &c, 1) > 0) { write(1, &c, 1); if (c == '\n') out++; }
    if (fd) close(fd);
    return 0;
}
static int ap_tail(int argc, char **argv) {
    int n = 10, i = 1;
    if (i < argc && !strcmp(argv[i], "-n") && i + 1 < argc) { n = atoi(argv[i + 1]); i += 2; }
    int fd = in_fd(argc, argv, i, "tail"); if (fd < 0) return 1;
    static char buf[65536]; int len = 0, r; char b[512];
    while ((r = read(fd, b, sizeof b)) > 0) for (int k = 0; k < r && len < (int)sizeof buf; k++) buf[len++] = b[k];
    if (fd) close(fd);
    int start = 0, nl = 0;
    for (int k = len - 1; k >= 0; k--) if (buf[k] == '\n' && k != len - 1) { if (++nl >= n) { start = k + 1; break; } }
    write(1, buf + start, len - start);
    return 0;
}
static int ap_rev(int argc, char **argv) {
    int fd = in_fd(argc, argv, 1, "rev"); if (fd < 0) return 1;
    char line[1024]; int li = 0; char c;
    for (;;) { int r = read(fd, &c, 1); if (r <= 0 && li == 0) break;
        if (c == '\n' || r <= 0) { for (int k = li - 1; k >= 0; k--) write(1, &line[k], 1); write(1, "\n", 1); li = 0; if (r <= 0) break; }
        else if (li < 1023) line[li++] = c; }
    if (fd) close(fd);
    return 0;
}
static int ap_tee(int argc, char **argv) {
    int fds[8], nf = 0;
    for (int i = 1; i < argc && nf < 8; i++) { int fd = open(argv[i], O_WRONLY | O_CREAT | O_TRUNC); if (fd >= 0) fds[nf++] = fd; }
    char b[512]; int n;
    while ((n = read(0, b, sizeof b)) > 0) { write(1, b, n); for (int i = 0; i < nf; i++) write(fds[i], b, n); }
    for (int i = 0; i < nf; i++) close(fds[i]);
    return 0;
}
static int ap_nl(int argc, char **argv) {
    int fd = in_fd(argc, argv, 1, "nl"); if (fd < 0) return 1;
    long ln = 0; char c; int start = 1;
    for (;;) { int r = read(fd, &c, 1); if (r <= 0) break; if (start) { ln++; outnum(1, ln); write(1, "\t", 1); start = 0; } write(1, &c, 1); if (c == '\n') start = 1; }
    if (fd) close(fd);
    return 0;
}
static int ap_yes(int argc, char **argv) {
    const char *s = argc > 1 ? argv[1] : "y";
    for (;;) { if (write(1, s, strlen(s)) <= 0) return 0; if (write(1, "\n", 1) <= 0) return 0; }
}
static int ap_seq(int argc, char **argv) {
    long a = 1, b;
    if (argc == 2) b = atol(argv[1]); else if (argc >= 3) { a = atol(argv[1]); b = atol(argv[2]); } else return 1;
    for (long v = a; v <= b; v++) { outnum(1, v); write(1, "\n", 1); }
    return 0;
}
static int ap_sort(int argc, char **argv) {
    int rev = argc > 1 && !strcmp(argv[1], "-r");
    static char buf[65536]; int len = 0; char c;
    while (read(0, &c, 1) > 0) if (len < (int)sizeof buf - 1) buf[len++] = c;
    buf[len] = 0;
    char *lines[2048]; int nl = 0; char *p = buf;
    while (*p && nl < 2048) { lines[nl++] = p; char *q = strchr(p, '\n'); if (!q) break; *q = 0; p = q + 1; }
    for (int i = 0; i < nl; i++) for (int j = i + 1; j < nl; j++) { int cmp = strcmp(lines[i], lines[j]); if ((rev && cmp < 0) || (!rev && cmp > 0)) { char *t = lines[i]; lines[i] = lines[j]; lines[j] = t; } }
    for (int i = 0; i < nl; i++) { out1(lines[i]); write(1, "\n", 1); }
    return 0;
}
static int ap_uniq(int argc, char **argv) {
    (void)argc; (void)argv;
    char cur[1024], prev[1024]; int ci = 0, first = 1; prev[0] = 0; char c;
    for (;;) { int r = read(0, &c, 1); if (c == '\n' || r <= 0) { cur[ci] = 0; if (first || strcmp(cur, prev)) { out1(cur); write(1, "\n", 1); strcpy(prev, cur); first = 0; } ci = 0; if (r <= 0) break; } else if (ci < 1023) cur[ci++] = c; }
    return 0;
}
static int ap_tr(int argc, char **argv) {
    if (argc < 3) { outs(2, "usage: tr SET1 SET2\n"); return 1; }
    const char *s1 = argv[1], *s2 = argv[2]; int l2 = strlen(s2); char c;
    while (read(0, &c, 1) > 0) { const char *m = strchr(s1, c); if (m && *m) { int idx = (int)(m - s1); char o = l2 ? s2[idx < l2 ? idx : l2 - 1] : c; if (l2) write(1, &o, 1); } else write(1, &c, 1); }
    return 0;
}

/* ---- files --------------------------------------------------------------- */
static void ls_print(const char *name, const struct stat *st, int lng) {
    if (lng) {
        out1(S_ISDIR(st->st_mode) ? "drwxr-xr-x 1 root root " : "-rw-r--r-- 1 root root ");
        outnum(1, (long)st->st_size); write(1, "\t", 1);
    }
    out1(name);
    write(1, lng ? "\n" : "  ", lng ? 1 : 2);
}
static int ap_ls(int argc, char **argv) {
    int all = 0, lng = 0, i = 1;
    for (; i < argc && argv[i][0] == '-'; i++)
        for (const char *f = argv[i] + 1; *f; f++) { if (*f == 'a') all = 1; else if (*f == 'l') lng = 1; }
    int first = i, last = i < argc ? argc : i + 1, rc = 0;
    for (; i < last; i++) {
        const char *path = first < argc ? argv[i] : ".";
        struct stat st;
        if (stat(path, &st) != 0) {
            outs(2, "ls: "); outs(2, path); outs(2, ": not found\n"); rc = 1; continue;
        }
        if (!S_ISDIR(st.st_mode)) { ls_print(path, &st, lng); continue; }
        DIR *d = opendir(path);
        if (!d) { outs(2, "ls: "); outs(2, path); outs(2, ": cannot open\n"); rc = 1; continue; }
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!all && e->d_name[0] == '.') continue;
            char full[288]; int n = 0;
            for (const char *p = path; *p && n < 200; p++) full[n++] = *p;
            if (n && full[n - 1] != '/') full[n++] = '/';
            for (const char *p = e->d_name; *p && n < 280; p++) full[n++] = *p;
            full[n] = 0;
            struct stat entry;
            if (stat(full, &entry) != 0) memset(&entry, 0, sizeof entry);
            ls_print(e->d_name, &entry, lng);
        }
        closedir(d);
    }
    if (!lng) write(1, "\n", 1);
    return rc;
}
struct tree_entry { char *name; int isdir; };
struct tree_opts { int all, dirs_only, max_depth, dirs, files, rc; };
static void tree_join(char *out, int cap, const char *dir, const char *name) {
    int n = 0;
    for (const char *p = dir; *p && n < cap - 2; p++) out[n++] = *p;
    if (n && out[n - 1] != '/') out[n++] = '/';
    for (const char *p = name; *p && n < cap - 1; p++) out[n++] = *p;
    out[n] = 0;
}
static int tree_cmp(const void *a, const void *b) {
    const struct tree_entry *ea = a, *eb = b;
    return strcmp(ea->name, eb->name);
}
static void tree_indent(const unsigned char *last, int depth, int is_last) {
    for (int i = 0; i < depth; i++) out1(last[i] ? "    " : "|   ");
    out1(is_last ? "`-- " : "|-- ");
}
static void tree_walk(const char *path, int depth, unsigned char *last, struct tree_opts *opt) {
    if (depth >= opt->max_depth) return;
    DIR *dir = opendir(path);
    if (!dir) { outs(2, "tree: cannot open "); outs(2, path); outs(2, "\n"); opt->rc = 1; return; }
    struct tree_entry *entries = 0; int count = 0, capacity = 0;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (!opt->all && entry->d_name[0] == '.') continue;
        char full[512]; tree_join(full, sizeof full, path, entry->d_name);
        struct stat st; int isdir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
        if (opt->dirs_only && !isdir) continue;
        if (count == capacity) {
            int next = capacity ? capacity * 2 : 16;
            struct tree_entry *grown = realloc(entries, (size_t)next * sizeof *entries);
            if (!grown) { opt->rc = 1; break; }
            entries = grown; capacity = next;
        }
        entries[count].name = strdup(entry->d_name);
        if (!entries[count].name) { opt->rc = 1; break; }
        entries[count++].isdir = isdir;
    }
    closedir(dir);
    qsort(entries, (size_t)count, sizeof *entries, tree_cmp);
    for (int i = 0; i < count; i++) {
        int is_last = i == count - 1;
        tree_indent(last, depth, is_last); out1(entries[i].name); write(1, "\n", 1);
        char full[512]; tree_join(full, sizeof full, path, entries[i].name);
        if (entries[i].isdir) {
            opt->dirs++; last[depth] = (unsigned char)is_last;
            tree_walk(full, depth + 1, last, opt);
        } else opt->files++;
        free(entries[i].name);
    }
    free(entries);
}
static int ap_tree(int argc, char **argv) {
    struct tree_opts opt = { 0, 0, 64, 0, 0, 0 };
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        if (!strcmp(argv[i], "-a")) opt.all = 1;
        else if (!strcmp(argv[i], "-d")) opt.dirs_only = 1;
        else if (!strcmp(argv[i], "-L")) {
            if (++i >= argc || (opt.max_depth = atoi(argv[i])) < 1) {
                outs(2, "tree: -L requires a positive depth\n"); return 1;
            }
        } else { outs(2, "usage: tree [-a] [-d] [-L depth] [path]\n"); return 1; }
    }
    if (argc - i > 1) { outs(2, "tree: only one path may be specified\n"); return 1; }
    const char *path = i < argc ? argv[i] : ".";
    struct stat st;
    if (stat(path, &st) != 0) { outs(2, "tree: "); outs(2, path); outs(2, ": not found\n"); return 1; }
    out1(path); write(1, "\n", 1);
    if (S_ISDIR(st.st_mode)) {
        unsigned char last[64] = { 0 };
        tree_walk(path, 0, last, &opt);
    } else if (!opt.dirs_only) opt.files = 1;
    out1("\n"); outnum(1, opt.dirs); out1(opt.dirs == 1 ? " directory, " : " directories, ");
    outnum(1, opt.files); out1(opt.files == 1 ? " file\n" : " files\n");
    return opt.rc;
}
/* join dir + '/' + name -> out (bounded) */
static void cp_join(char *out, int cap, const char *dir, const char *name) {
    int n = 0;
    for (const char *p = dir; *p && n < cap - 2; p++) out[n++] = *p;
    if (n && out[n - 1] != '/') out[n++] = '/';
    for (const char *p = name; *p && n < cap - 1; p++) out[n++] = *p;
    out[n] = 0;
}
static const char *cp_base(const char *p) {
    const char *b = p;
    for (const char *q = p; *q; q++) if (*q == '/') b = q + 1;
    return b;
}
static int cp_is_dir(const char *p) {
    struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}
/* copy one regular file src -> dst (full paths); routes across /, /mnt, /usb */
static int cp_file(const char *src, const char *dst) {
    int s = open(src, O_RDONLY);
    if (s < 0) { outs(2, "cp: cannot open "); outs(2, src); outs(2, "\n"); return 1; }
    int d = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (d < 0) { close(s); outs(2, "cp: cannot create "); outs(2, dst); outs(2, "\n"); return 1; }
    char b[512]; int n, rc = 0;
    while ((n = read(s, b, sizeof b)) > 0) if (write(d, b, n) != n) { rc = 1; break; }
    if (n < 0) rc = 1;
    close(s); close(d);
    return rc;
}
/* copy src -> dst; recurse into directories when rflag is set */
static int cp_tree(const char *src, const char *dst, int rflag) {
    struct stat st;
    if (stat(src, &st) != 0) { outs(2, "cp: "); outs(2, src); outs(2, ": not found\n"); return 1; }
    if (!S_ISDIR(st.st_mode)) return cp_file(src, dst);
    if (!rflag) { outs(2, "cp: omitting directory "); outs(2, src); outs(2, " (use -r)\n"); return 1; }
    if (mkdir(dst, 0755) != 0 && !cp_is_dir(dst)) {
        outs(2, "cp: cannot create directory "); outs(2, dst); outs(2, "\n"); return 1;
    }
    DIR *dir = opendir(src);
    if (!dir) { outs(2, "cp: cannot read "); outs(2, src); outs(2, "\n"); return 1; }
    struct dirent *e; int rc = 0;
    while ((e = readdir(dir))) {
        if (e->d_name[0] == '.' && (e->d_name[1] == 0 || (e->d_name[1] == '.' && e->d_name[2] == 0))) continue;
        char sp[288], dp[288];
        cp_join(sp, sizeof sp, src, e->d_name);
        cp_join(dp, sizeof dp, dst, e->d_name);
        if (cp_tree(sp, dp, rflag)) rc = 1;
    }
    closedir(dir);
    return rc;
}
/* cp [-r] SRC... DST  — like coreutils cp: recursive, multiple sources into a
 * directory, and works across the /, /mnt and /usb filesystems. */
static int ap_cp(int argc, char **argv) {
    int rflag = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
        for (const char *f = argv[i] + 1; *f; f++) {
            if (*f == 'r' || *f == 'R' || *f == 'a') rflag = 1;
            else { outs(2, "cp: unknown option -"); write(2, f, 1); outs(2, "\n"); return 1; }
        }
    if (argc - i < 2) { outs(2, "usage: cp [-r] SRC... DST\n"); return 1; }
    const char *dst = argv[argc - 1];
    int into_dir = cp_is_dir(dst);
    if (argc - i > 2 && !into_dir) { outs(2, "cp: target '"); outs(2, dst); outs(2, "' is not a directory\n"); return 1; }
    int rc = 0;
    for (int k = i; k < argc - 1; k++) {
        if (into_dir) {
            char dp[288]; cp_join(dp, sizeof dp, dst, cp_base(argv[k]));
            if (cp_tree(argv[k], dp, rflag)) rc = 1;
        } else if (cp_tree(argv[k], dst, rflag)) rc = 1;
    }
    return rc;
}
/* mv [-r] SRC... DST  — copy then remove the sources (files only). */
static int ap_mv(int argc, char **argv) {
    if (ap_cp(argc, argv)) return 1;
    int i = 1; for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {}
    for (int k = i; k < argc - 1; k++) unlink(argv[k]);
    return 0;
}
static int ap_rm(int argc, char **argv) {
    int rc = 0, i = 1; while (i < argc && argv[i][0] == '-') i++;
    for (; i < argc; i++) if (unlink(argv[i]) < 0) { outs(2, "rm: "); outs(2, argv[i]); outs(2, ": failed\n"); rc = 1; }
    return rc;
}
static int ap_mkdir(int argc, char **argv) {
    int rc = 0, i = 1; while (i < argc && argv[i][0] == '-') i++;
    for (; i < argc; i++) if (mkdir(argv[i], 0755) < 0) { outs(2, "mkdir: "); outs(2, argv[i]); outs(2, ": failed\n"); rc = 1; }
    return rc;
}
static int ap_rmdir(int argc, char **argv) { int rc = 0; for (int i = 1; i < argc; i++) if (rmdir(argv[i]) < 0) rc = 1; return rc; }
static int ap_touch(int argc, char **argv) {
    for (int i = 1; i < argc; i++) { int fd = open(argv[i], O_WRONLY | O_CREAT); if (fd >= 0) close(fd); }
    return 0;
}
static int ap_cmp(int argc, char **argv) {
    if (argc < 3) return 2;
    int a = open(argv[1], O_RDONLY), b = open(argv[2], O_RDONLY);
    if (a < 0 || b < 0) { outs(2, "cmp: open failed\n"); return 2; }
    char ca, cb; long pos = 0; int diff = 0;
    for (;;) { int ra = read(a, &ca, 1), rb = read(b, &cb, 1); if (ra <= 0 && rb <= 0) break; pos++; if (ra != rb || ca != cb) { diff = 1; break; } }
    close(a); close(b);
    if (diff) { out1("differ at byte "); outnum(1, pos); write(1, "\n", 1); return 1; }
    return 0;
}
static int ap_stat(int argc, char **argv) {
    if (argc < 2) return 1;
    struct stat st; if (stat(argv[1], &st) < 0) { outs(2, "stat: not found\n"); return 1; }
    out1("  File: "); out1(argv[1]);
    out1(S_ISDIR(st.st_mode) ? "  (directory)\n  Size: " : "  (regular file)\n  Size: ");
    outnum(1, (long)st.st_size); write(1, "\n", 1);
    return 0;
}
static int ap_basename(int argc, char **argv) {
    if (argc < 2) return 1; const char *b = argv[1]; for (const char *p = argv[1]; *p; p++) if (*p == '/') b = p + 1;
    out1(b); write(1, "\n", 1); return 0;
}
static int ap_dirname(int argc, char **argv) {
    if (argc < 2) return 1; char d[288]; int last = -1, n = 0;
    for (const char *p = argv[1]; *p && n < 280; p++) { d[n] = *p; if (*p == '/') last = n; n++; }
    if (last < 0) out1("."); else if (last == 0) out1("/"); else { d[last] = 0; out1(d); }
    write(1, "\n", 1); return 0;
}

/* ---- system -------------------------------------------------------------- */
static int ap_pwd(int argc, char **argv) { (void)argc; (void)argv; char b[256]; if (getcwd(b, sizeof b)) { out1(b); write(1, "\n", 1); } else out1("/\n"); return 0; }
static int ap_uname(int argc, char **argv) {
    struct utsname u; uname(&u);
    if (argc > 1 && !strcmp(argv[1], "-a")) { out1(u.sysname); write(1, " ", 1); out1(u.nodename); write(1, " ", 1); out1(u.release); write(1, " ", 1); out1(u.machine); }
    else if (argc > 1 && !strcmp(argv[1], "-r")) out1(u.release);
    else if (argc > 1 && !strcmp(argv[1], "-m")) out1(u.machine);
    else out1(u.sysname);
    write(1, "\n", 1); return 0;
}
static int ap_sleep(int argc, char **argv) { if (argc < 2) return 1; struct timespec ts = { atol(argv[1]), 0 }; nanosleep(&ts, 0); return 0; }
static int ap_date(int argc, char **argv) {
    (void)argc; (void)argv;
    time_t t = time(0);
    struct tm tm; gmtime_r(&t, &tm);
    char b[24]; int n = 0, y = tm.tm_year + 1900;
    b[n++] = '0' + (y / 1000) % 10; b[n++] = '0' + (y / 100) % 10;
    b[n++] = '0' + (y / 10) % 10;   b[n++] = '0' + y % 10;
    b[n++] = '-'; b[n++] = '0' + (tm.tm_mon + 1) / 10; b[n++] = '0' + (tm.tm_mon + 1) % 10;
    b[n++] = '-'; b[n++] = '0' + tm.tm_mday / 10; b[n++] = '0' + tm.tm_mday % 10;
    b[n++] = ' '; b[n++] = '0' + tm.tm_hour / 10; b[n++] = '0' + tm.tm_hour % 10;
    b[n++] = ':'; b[n++] = '0' + tm.tm_min / 10;  b[n++] = '0' + tm.tm_min % 10;
    b[n++] = ':'; b[n++] = '0' + tm.tm_sec / 10;  b[n++] = '0' + tm.tm_sec % 10;
    b[n++] = '\n'; write(1, b, n);
    return 0;
}
static int ap_id(int argc, char **argv) { (void)argc; (void)argv; out1("uid=0(root) gid=0(root)\n"); return 0; }
static int ap_whoami(int argc, char **argv) { (void)argc; (void)argv; out1("root\n"); return 0; }
static int ap_hostname(int argc, char **argv) { (void)argc; (void)argv; out1("tinyos\n"); return 0; }
static int ap_env(int argc, char **argv) { (void)argc; (void)argv; for (char **e = environ; e && *e; e++) { out1(*e); write(1, "\n", 1); } return 0; }
static int ap_clear(int argc, char **argv) { (void)argc; (void)argv; out1("\033[2J\033[H"); return 0; }
static int ap_free(int argc, char **argv) { (void)argc; (void)argv; int fd = open("/proc/meminfo", O_RDONLY); if (fd < 0) return 1; catfd(fd); close(fd); return 0; }
static int ap_sync(int argc, char **argv) { (void)argc; (void)argv; return 0; }
static int ap_true(int argc, char **argv) { (void)argc; (void)argv; return 0; }
static int ap_false(int argc, char **argv) { (void)argc; (void)argv; return 1; }
static int ap_kill(int argc, char **argv) {
    int sig = 15, i = 1;
    if (i < argc && argv[i][0] == '-') { sig = atoi(argv[i] + 1); if (!sig) sig = 15; i++; }
    for (; i < argc; i++) kill(atoi(argv[i]), sig);
    return 0;
}
static int ap_test(int argc, char **argv) {
    int n = argc;
    if (!strcmp(argv[0], "[") && n > 1 && !strcmp(argv[n - 1], "]")) n--;
    if (n == 2) return argv[1][0] ? 0 : 1;
    if (n == 3) {
        const char *op = argv[1], *a = argv[2]; struct stat st;
        if (!strcmp(op, "-e")) return stat(a, &st) == 0 ? 0 : 1;
        if (!strcmp(op, "-f")) return (stat(a, &st) == 0 && S_ISREG(st.st_mode)) ? 0 : 1;
        if (!strcmp(op, "-d")) return (stat(a, &st) == 0 && S_ISDIR(st.st_mode)) ? 0 : 1;
        if (!strcmp(op, "-z")) return a[0] == 0 ? 0 : 1;
        if (!strcmp(op, "-n")) return a[0] ? 0 : 1;
        return 1;
    }
    if (n == 4) {
        const char *a = argv[1], *op = argv[2], *b = argv[3];
        if (!strcmp(op, "="))  return strcmp(a, b) == 0 ? 0 : 1;
        if (!strcmp(op, "!=")) return strcmp(a, b) != 0 ? 0 : 1;
        long x = atol(a), y = atol(b);
        if (!strcmp(op, "-eq")) return x == y ? 0 : 1;
        if (!strcmp(op, "-ne")) return x != y ? 0 : 1;
        if (!strcmp(op, "-lt")) return x <  y ? 0 : 1;
        if (!strcmp(op, "-le")) return x <= y ? 0 : 1;
        if (!strcmp(op, "-gt")) return x >  y ? 0 : 1;
        if (!strcmp(op, "-ge")) return x >= y ? 0 : 1;
    }
    return 1;
}
static int ap_expr(int argc, char **argv) {
    if (argc != 4) return 2;
    long a = atol(argv[1]), b = atol(argv[3]); const char *op = argv[2]; long r;
    if (!strcmp(op, "+")) r = a + b; else if (!strcmp(op, "-")) r = a - b;
    else if (!strcmp(op, "*")) r = a * b; else if (!strcmp(op, "/")) r = b ? a / b : 0;
    else if (!strcmp(op, "%")) r = b ? a % b : 0; else return 2;
    outnum(1, r); write(1, "\n", 1); return r ? 0 : 1;
}

/* ---- vi: modal full-screen editor (parity with the native programs/vi.cpp) --
 * Normal: h j k l / arrows move; 0 $ line ends; i a A insert; o open; x del;
 * dd del line; : ex (:w :q :wq :x :q!).  Insert: type, Tab, Enter, Backspace,
 * arrows, ESC.  Line-number gutter + long-line wrapping like the native vi. */
#define VI_MAXL (512 * 3)
#define VI_MAXC (200 * 2)
#define VI_GUT  5
#define VK_UP 0x80
#define VK_DOWN 0x81
#define VK_LEFT 0x82
#define VK_RIGHT 0x83
static char vbuf[VI_MAXL][VI_MAXC + 2];
static int  vlen[VI_MAXL], vnl, vcx, vcy, vtop, vVH = 23, vVW = 75, vdirty;
static char vfname[128];

static void vout(const char *s) { int n = 0; while (s[n]) n++; if (n) write(1, s, n); }
static void vnum(int v) {
    char t[10]; int n = 0; if (!v) t[n++] = '0'; while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    char r[10]; int m = 0; while (n) r[m++] = t[--n]; write(1, r, m);
}
static void vxy(int row, int col) { vout("\x1b["); vnum(row); vout(";"); vnum(col); vout("H"); }
static int  vi_rows(int i) { int n = (vlen[i] + vVW - 1) / vVW; return n < 1 ? 1 : n; }

static void vi_load(const char *path) {
    vnl = 0;
    int fd = path[0] ? open(path, O_RDONLY) : -1;
    if (fd >= 0) {
        static char d[VI_MAXL * 40]; int n = read(fd, d, sizeof d - 1); close(fd);
        if (n < 0) n = 0;
        int c = 0;
        for (int i = 0; i < n && vnl < VI_MAXL; i++) {
            if (d[i] == '\n') { vlen[vnl] = c; vbuf[vnl][c] = 0; vnl++; c = 0; }
            else if (d[i] != '\r' && c < VI_MAXC) vbuf[vnl][c++] = d[i];
        }
        if (vnl < VI_MAXL && c > 0) { vlen[vnl] = c; vbuf[vnl][c] = 0; vnl++; }
    }
    if (vnl == 0) { vnl = 1; vlen[0] = 0; vbuf[0][0] = 0; }
}

static int vi_save(void) {
    if (!vfname[0]) return -1;
    int fd = open(vfname, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    for (int i = 0; i < vnl; i++) { if (vlen[i]) write(fd, vbuf[i], vlen[i]); write(fd, "\n", 1); }
    close(fd); vdirty = 0; return 0;
}

static void vi_ins(char c) {
    if (vlen[vcy] >= VI_MAXC) return;
    for (int i = vlen[vcy]; i > vcx; i--) vbuf[vcy][i] = vbuf[vcy][i - 1];
    vbuf[vcy][vcx] = c; vlen[vcy]++; vbuf[vcy][vlen[vcy]] = 0; vcx++; vdirty = 1;
}
static void vi_del(void) {
    if (vcx >= vlen[vcy]) return;
    for (int i = vcx; i < vlen[vcy] - 1; i++) vbuf[vcy][i] = vbuf[vcy][i + 1];
    vlen[vcy]--; vbuf[vcy][vlen[vcy]] = 0; vdirty = 1;
}
static void vi_sdown(int from) { for (int i = vnl; i > from; i--) { memcpy(vbuf[i], vbuf[i - 1], VI_MAXC + 2); vlen[i] = vlen[i - 1]; } vnl++; }
static void vi_sup(int at)     { for (int i = at; i < vnl - 1; i++) { memcpy(vbuf[i], vbuf[i + 1], VI_MAXC + 2); vlen[i] = vlen[i + 1]; } vnl--; }
static void vi_split(void) {
    if (vnl >= VI_MAXL) return;
    vi_sdown(vcy + 1);
    int m = 0; for (int j = vcx; j < vlen[vcy]; j++) vbuf[vcy + 1][m++] = vbuf[vcy][j];
    vlen[vcy + 1] = m; vbuf[vcy + 1][m] = 0; vlen[vcy] = vcx; vbuf[vcy][vcx] = 0;
    vcy++; vcx = 0; vdirty = 1;
}
static void vi_openl(void) { if (vnl >= VI_MAXL) return; vi_sdown(vcy + 1); vcy++; vlen[vcy] = 0; vbuf[vcy][0] = 0; vcx = 0; vdirty = 1; }
static void vi_bs(void) {
    if (vcx > 0) { vcx--; vi_del(); }
    else if (vcy > 0 && vlen[vcy - 1] + vlen[vcy] < VI_MAXC) {
        int pl = vlen[vcy - 1], save = vcy;
        for (int i = 0; i < vlen[vcy]; i++) vbuf[vcy - 1][pl + i] = vbuf[vcy][i];
        vlen[vcy - 1] = pl + vlen[vcy]; vbuf[vcy - 1][vlen[vcy - 1]] = 0;
        vcy--; vcx = pl; vi_sup(save); vdirty = 1;
    }
}
static void vi_ddl(void) {
    if (vnl > 1) { vi_sup(vcy); if (vcy >= vnl) vcy = vnl - 1; }
    else { vlen[0] = 0; vbuf[0][0] = 0; }
    vcx = 0; vdirty = 1;
}

static void vi_redraw(const char *status) {
    vout("\x1b[2J");
    int r = 0, i = vtop;
    while (r < vVH && i < vnl) {
        int rows = vi_rows(i);
        for (int k = 0; k < rows && r < vVH; k++, r++) {
            char num[VI_GUT];
            if (k == 0) { int ln = i + 1;
                num[0] = (ln >= 1000) ? (char)('0' + (ln / 1000) % 10) : ' ';
                num[1] = (ln >= 100)  ? (char)('0' + (ln / 100) % 10)  : ' ';
                num[2] = (ln >= 10)   ? (char)('0' + (ln / 10) % 10)   : ' ';
                num[3] = (char)('0' + ln % 10); num[4] = ' ';
            } else { num[0] = num[1] = num[2] = num[3] = num[4] = ' '; }
            vxy(r + 1, 1); write(1, num, VI_GUT);
            int start = k * vVW, n = vlen[i] - start; if (n > vVW) n = vVW;
            if (n > 0) write(1, vbuf[i] + start, n);
        }
        i++;
    }
    vxy(vVH + 1, 1); vout("\x1b[K"); vout(status);
    int sr = 0; for (int j = vtop; j < vcy; j++) sr += vi_rows(j); sr += vcx / vVW;
    vxy(sr + 1, VI_GUT + (vcx % vVW) + 1);
}

static int ap_vi(int argc, char **argv) {
    struct winsize ws;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 2) { vVH = ws.ws_row - 1; if (ws.ws_col > VI_GUT + 4) vVW = ws.ws_col - VI_GUT; }
    vfname[0] = 0;
    if (argc > 1) { int j = 0; for (const char *p = argv[1]; *p && j < 127; p++) vfname[j++] = *p; vfname[j] = 0; }
    vi_load(vfname);
    vcx = vcy = vtop = vdirty = 0;

    struct termios oldt, raw;                  /* raw mode: keys one at a time, no echo */
    tcgetattr(0, &oldt); raw = oldt;
    raw.c_lflag &= ~(ICANON | ECHO | ISIG);
    tcsetattr(0, TCSANOW, &raw);

    int ins = 0, run = 1, count = 0;           /* count = pending numeric prefix, e.g. 10G */
    while (run) {
        char st[128]; int p = 0;               /* status line (native-style) */
        if (ins) { const char *m = "-- INSERT --  (ESC=normal)"; while (*m && p < 127) st[p++] = *m++; }
        else {
            st[p++] = '"'; for (const char *f = vfname[0] ? vfname : "[No Name]"; *f && p < 80; f++) st[p++] = *f;
            const char *m = vdirty ? "\" [modified]  " : "\"  "; while (*m && p < 108) st[p++] = *m++;
            m = "i insert  x del  o open  :w save  :q quit"; while (*m && p < 127) st[p++] = *m++;
        }
        st[p] = 0;
        vi_redraw(st);

        char rc; if (read(0, &rc, 1) <= 0) continue;
        int c = (unsigned char)rc;
        if (ins) {                             /* ---- INSERT ---- */
            if (c == 27) { ins = 0; if (vcx > 0) vcx--; }
            else if (c == '\n' || c == '\r') vi_split();
            else if (c == 8 || c == 127) vi_bs();
            else if (c == VK_LEFT)  { if (vcx > 0) vcx--; }
            else if (c == VK_RIGHT) { if (vcx < vlen[vcy]) vcx++; }
            else if (c == VK_UP)    { if (vcy > 0) { vcy--; if (vcx > vlen[vcy]) vcx = vlen[vcy]; } }
            else if (c == VK_DOWN)  { if (vcy < vnl - 1) { vcy++; if (vcx > vlen[vcy]) vcx = vlen[vcy]; } }
            else if (c == '\t') { int n = 4 - (vcx & 3); while (n-- > 0) vi_ins(' '); }
            else if (c >= 32 && c < 127) vi_ins((char)c);
        } else {                               /* ---- NORMAL ---- */
            if ((c >= '1' && c <= '9') || (c == '0' && count > 0)) { count = count * 10 + (c - '0'); continue; }
            int cnt = count; count = 0;        /* consume any pending [count] for this command */
            if (c == 'h' || c == VK_LEFT) { if (vcx > 0) vcx--; }
            else if (c == 'l' || c == VK_RIGHT) { if (vcx < vlen[vcy]) vcx++; }
            else if (c == 'j' || c == VK_DOWN) { if (vcy < vnl - 1) { vcy++; if (vcx > vlen[vcy]) vcx = vlen[vcy]; } }
            else if (c == 'k' || c == VK_UP) { if (vcy > 0) { vcy--; if (vcx > vlen[vcy]) vcx = vlen[vcy]; } }
            else if (c == '0') vcx = 0;
            else if (c == '$') vcx = vlen[vcy] ? vlen[vcy] - 1 : 0;
            else if (c == 'G') { vcy = cnt > 0 ? cnt - 1 : vnl - 1; if (vcy >= vnl) vcy = vnl - 1; if (vcy < 0) vcy = 0; vcx = 0; }
            else if (c == 'g') { char g2; if (read(0, &g2, 1) > 0 && g2 == 'g') { vcy = cnt > 0 ? cnt - 1 : 0; if (vcy >= vnl) vcy = vnl - 1; if (vcy < 0) vcy = 0; vcx = 0; } }
            else if (c == 'i') ins = 1;
            else if (c == 'a') { if (vcx < vlen[vcy]) vcx++; ins = 1; }
            else if (c == 'A') { vcx = vlen[vcy]; ins = 1; }
            else if (c == 'x') vi_del();
            else if (c == 'o') { vi_openl(); ins = 1; }
            else if (c == 'd') { char d2; if (read(0, &d2, 1) > 0 && d2 == 'd') vi_ddl(); }
            else if (c == ':') {
                char cb[64]; int cn = 0;
                for (;;) {
                    vxy(vVH + 1, 1); vout("\x1b[K:"); if (cn > 0) write(1, cb, cn);
                    char cc; if (read(0, &cc, 1) <= 0) continue;
                    int ic = (unsigned char)cc;
                    if (ic == '\n' || ic == '\r') break;
                    if (ic == 27) { cn = -1; break; }
                    if (ic == 8 || ic == 127) { if (cn > 0) cn--; continue; }
                    if (cn < 63 && ic >= 32 && ic < 127) cb[cn++] = (char)ic;
                }
                if (cn >= 0) { cb[cn] = 0;
                    int w = 0, q = 0, bang = 0;
                    for (int k = 0; cb[k]; k++) { if (cb[k] == 'w') w = 1; else if (cb[k] == 'q') q = 1; else if (cb[k] == '!') bang = 1; else if (cb[k] == 'x') w = q = 1; }
                    if (w && vi_save() < 0) { vxy(vVH + 1, 1); vout("\x1b[K"); vout("write failed"); read(0, &rc, 1); }
                    if (q && (w || bang || !vdirty)) run = 0;
                    else if (q) { vxy(vVH + 1, 1); vout("\x1b[K"); vout("unsaved (:q! to override)"); read(0, &rc, 1); }
                }
            }
        }
        if (vcx > vlen[vcy]) vcx = vlen[vcy];
        if (vcy < vtop) vtop = vcy;            /* wrap-aware scroll to keep the cursor visible */
        for (;;) { int sr = 0; for (int j = vtop; j < vcy; j++) sr += vi_rows(j); sr += vcx / vVW; if (sr < vVH || vtop >= vcy) break; vtop++; }
    }
    tcsetattr(0, TCSANOW, &oldt);
    vout("\x1b[2J\x1b[H");
    return 0;
}

static int run_shell(int argc, char **argv);
static int ap_help(int argc, char **argv);

/* ---- net: ping + a tiny text-mode web browser ---------------------------- */
/* case-insensitive prefix compare: 0 when a[0..n) equals b[0..n) */
static int ncase(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) { int ca = tolower((unsigned char)a[i]), cb = tolower((unsigned char)b[i]); if (ca != cb) return 1; if (!ca) return 0; }
    return 0;
}
static int starts(const char *s, const char *pfx) { return ncase(s, pfx, (int)strlen(pfx)) == 0; }

/* dotted-quad literal or DNS name -> IPv4 in host order (0 on failure) */
static uint32_t resolve_host(const char *h) {
    uint32_t lit = ntohl(inet_addr(h));
    if (lit != 0xFFFFFFFFu && lit != 0) return lit;
    return net_resolve(h);
}
static void put_ip(int fd, uint32_t ip) {
    outnum(fd, (ip >> 24) & 255); write(fd, ".", 1); outnum(fd, (ip >> 16) & 255); write(fd, ".", 1);
    outnum(fd, (ip >> 8) & 255);  write(fd, ".", 1); outnum(fd, ip & 255);
}
static void put_hex2(int fd, unsigned v) {
    static const char h[] = "0123456789abcdef";
    char b[2] = { h[(v >> 4) & 15], h[v & 15] }; write(fd, b, 2);
}

/* ping [-c count] host  — ICMP echo via the kernel (NET_SYS_PING). */
static int ap_ping(int argc, char **argv) {
    int count = 4, i = 1;
    for (; i < argc && argv[i][0] == '-'; i++)
        if (!strcmp(argv[i], "-c") && i + 1 < argc) { count = atoi(argv[++i]); if (count <= 0) count = 4; }
    if (i >= argc) { outs(2, "usage: ping [-c count] host\n"); return 2; }
    const char *host = argv[i];
    uint32_t ip = resolve_host(host);
    if (!ip) { outs(2, "ping: cannot resolve "); outs(2, host); outs(2, "\n"); return 1; }
    out1("PING "); out1(host); out1(" ("); put_ip(1, ip); out1(")\n");
    int rx = 0;
    for (int s = 1; s <= count; s++) {
        long us = net_ping(ip);
        if (us >= 0) {
            rx++;
            out1("reply from "); put_ip(1, ip); out1(": seq="); outnum(1, s);
            out1(" time="); outnum(1, us / 1000); write(1, ".", 1); outnum(1, (us % 1000) / 100); out1(" ms\n");
        } else { out1("request timeout: seq="); outnum(1, s); write(1, "\n", 1); }
        if (s < count) { struct timespec ts = { 1, 0 }; nanosleep(&ts, 0); }
    }
    out1("--- "); out1(host); out1(" ping statistics ---\n");
    outnum(1, count); out1(" transmitted, "); outnum(1, rx); out1(" received\n");
    return rx > 0 ? 0 : 1;
}

/* ifconfig [ip mask gw [dns]] — show, or set a static interface config. */
static int ap_ifconfig(int argc, char **argv) {
    if (argc >= 4) {                                  /* static config from dotted-quad args */
        uint32_t ip = ntohl(inet_addr(argv[1])), mask = ntohl(inet_addr(argv[2]));
        uint32_t gw = ntohl(inet_addr(argv[3])), dns = argc > 4 ? ntohl(inet_addr(argv[4])) : gw;
        if (net_setcfg(ip, mask, gw, dns) != 0) { outs(2, "ifconfig: set failed\n"); return 1; }
    }
    struct tnet_ifcfg c;
    if (net_ifconfig(&c) != 0) { outs(2, "ifconfig: network unavailable\n"); return 1; }
    out1("eth0      Link encap:Ethernet  HWaddr ");
    for (int i = 0; i < 6; i++) { if (i) write(1, ":", 1); put_hex2(1, c.mac[i]); }
    write(1, "\n", 1);
    out1("          inet "); put_ip(1, c.ip); out1("  netmask "); put_ip(1, c.mask);
    out1("  gateway "); put_ip(1, c.gw); write(1, "\n", 1);
    out1("          dns "); put_ip(1, c.dns); out1("  MTU 1500  status ");
    out1(c.up ? "UP\n" : "DOWN\n");
    out1("lo        Link encap:Local Loopback\n");
    out1("          inet 127.0.0.1  netmask 255.0.0.0  status UP\n");
    return 0;
}

/* --- text web browser: fetch HTTP and render HTML as flowing text --- */
#define WEB_CAP  (128 * 1024)
#define WEB_MAXL 128
#define WEB_URLM 512
static char web_buf[WEB_CAP];
static char web_host[256];
static int  web_port;
static char web_path[WEB_URLM];
static char web_links[WEB_MAXL][WEB_URLM];
static int  web_nlink;
static int  web_pnl, web_psp;          /* pending newlines / pending space (whitespace collapse) */

static void web_emit(char c) {
    if (c == '\n') { if (web_pnl < 2) web_pnl++; web_psp = 0; return; }
    if (c == ' ' || c == '\t' || c == '\r') { web_psp = 1; return; }
    while (web_pnl > 0) { write(1, "\n", 1); web_pnl--; }
    if (web_psp) { write(1, " ", 1); web_psp = 0; }
    write(1, &c, 1);
}
static void web_emits(const char *s) { while (*s) web_emit(*s++); }

static int web_is_block(const char *n) {
    static const char *b[] = { "p", "br", "div", "li", "tr", "ul", "ol", "hr", "h1", "h2", "h3", "h4",
                               "h5", "h6", "table", "thead", "tbody", "section", "article", "header",
                               "footer", "nav", "blockquote", "pre", "form", "dl", "dt", "dd", 0 };
    for (int i = 0; b[i]; i++) if (!strcmp(n, b[i])) return 1;
    return 0;
}
static void web_entity(const char *e) {
    static const struct { const char *k, *v; } t[] = {
        { "amp", "&" }, { "lt", "<" }, { "gt", ">" }, { "quot", "\"" }, { "apos", "'" },
        { "nbsp", " " }, { "copy", "(c)" }, { "reg", "(r)" }, { "trade", "(tm)" }, { "mdash", "--" },
        { "ndash", "-" }, { "hellip", "..." }, { "raquo", ">>" }, { "laquo", "<<" }, { "middot", "*" }, { 0, 0 } };
    if (e[0] == '#') {                                     /* numeric entity */
        long cp = (e[1] == 'x' || e[1] == 'X') ? strtol(e + 2, 0, 16) : atol(e + 1);
        if (cp >= 32 && cp < 127) web_emit((char)cp);
        else if (cp == 160) web_emit(' ');
        else web_emit('?');
        return;
    }
    for (int i = 0; t[i].k; i++) if (!strcmp(e, t[i].k)) { web_emits(t[i].v); return; }
    web_emit('&'); web_emits(e); web_emit(';');           /* unknown: leave literal */
}
/* extract attribute `name`'s value from the tag body [s,end); 1 if found */
static int web_attr(const char *s, const char *end, const char *name, char *out, int cap) {
    int nl = (int)strlen(name);
    for (const char *p = s; p + nl < end; p++) {
        if ((p == s || !isalnum((unsigned char)p[-1])) && ncase(p, name, nl) == 0) {
            const char *q = p + nl; while (q < end && (*q == ' ' || *q == '\t')) q++;
            if (q < end && *q == '=') {
                q++; while (q < end && (*q == ' ' || *q == '\t')) q++;
                char quote = 0; if (q < end && (*q == '"' || *q == '\'')) quote = *q++;
                int o = 0;
                while (q < end && o < cap - 1) {
                    if (quote) { if (*q == quote) break; } else if (*q == ' ' || *q == '\t' || *q == '>') break;
                    out[o++] = *q++;
                }
                out[o] = 0; return 1;
            }
        }
    }
    return 0;
}
static void web_render(const char *body) {
    web_pnl = web_psp = 0; web_nlink = 0;
    int in_script = 0, in_style = 0, cur = -1;
    for (const char *p = body; *p; ) {
        if (*p == '<') {
            const char *t = p + 1; int close = (*t == '/'); if (close) t++;
            char name[16]; int ni = 0;
            while (*t && isalnum((unsigned char)*t) && ni < 15) name[ni++] = (char)tolower((unsigned char)*t++);
            name[ni] = 0;
            const char *e = p + 1; while (*e && *e != '>') e++;
            if (!strcmp(name, "script")) in_script = !close;
            else if (!strcmp(name, "style")) in_style = !close;
            else if (!in_script && !in_style && ni > 0) {
                if (!strcmp(name, "a")) {
                    if (!close) {
                        char href[WEB_URLM];
                        if (web_attr(p + 1, e, "href", href, sizeof href) && href[0] && href[0] != '#' && web_nlink < WEB_MAXL) {
                            strncpy(web_links[web_nlink], href, WEB_URLM - 1); web_links[web_nlink][WEB_URLM - 1] = 0;
                            cur = web_nlink++;
                        }
                    } else if (cur >= 0) { char m[16]; snprintf(m, sizeof m, "[%d]", cur + 1); web_emits(m); cur = -1; }
                } else if (web_is_block(name)) {
                    web_emit('\n');
                    if (!strcmp(name, "li") && !close) web_emits("- ");
                }
            }
            p = (*e == '>') ? e + 1 : e;
            continue;
        }
        if (in_script || in_style) { p++; continue; }
        if (*p == '&') {
            char ent[12]; int k = 0; const char *q = p + 1;
            while (*q && *q != ';' && *q != '<' && *q != ' ' && *q != '&' && k < 10) ent[k++] = *q++;
            ent[k] = 0;
            if (*q == ';' && k > 0) { web_entity(ent); p = q + 1; continue; }
            web_emit('&'); p++; continue;
        }
        web_emit(*p); p++;
    }
    write(1, "\n", 1);
}
/* split a URL into web_host/web_port/web_path; -1 for https or a malformed URL */
static int web_parse(const char *url) {
    const char *p = url;
    if (starts(p, "http://")) p += 7;
    else if (starts(p, "https://")) return -1;
    int hi = 0; web_port = 80;
    while (*p && *p != '/' && *p != ':' && hi < 255) web_host[hi++] = *p++;
    web_host[hi] = 0;
    if (!web_host[0]) return -1;
    if (*p == ':') { p++; web_port = atoi(p); while (*p && *p != '/') p++; }
    if (*p == '/') { int pi = 0; while (*p && pi < WEB_URLM - 1) web_path[pi++] = *p++; web_path[pi] = 0; }
    else strcpy(web_path, "/");
    return 0;
}
static int web_fetch(void) {                              /* GET web_path into web_buf; body length or -1 */
    uint32_t ip = resolve_host(web_host);
    if (!ip) return -1;
    int s = socket(AF_INET, SOCK_STREAM, 0); if (s < 0) return -1;
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((unsigned short)web_port); a.sin_addr.s_addr = htonl(ip);
    if (connect(s, (struct sockaddr *)&a, sizeof a) != 0) { close(s); return -1; }
    char req[2048];
    int rl = snprintf(req, sizeof req,
                      "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: tinyos-web/1.0\r\nConnection: close\r\n\r\n",
                      web_path, web_host);
    if (rl < 0) { close(s); return -1; }
    if (rl >= (int)sizeof req) rl = (int)sizeof req - 1;
    if (write(s, req, rl) != rl) { close(s); return -1; }
    int len = 0, n;
    while (len < WEB_CAP - 1 && (n = read(s, web_buf + len, WEB_CAP - 1 - len)) > 0) len += n;
    web_buf[len] = 0; close(s);
    return len;
}
/* case-insensitive header lookup within [hdr,end); 1 if found */
static int web_header(const char *hdr, const char *end, const char *name, char *out, int cap) {
    int nl = (int)strlen(name);
    for (const char *p = hdr; p < end; ) {
        const char *le = p; while (le < end && *le != '\n') le++;
        if (le - p > nl && ncase(p, name, nl) == 0 && p[nl] == ':') {
            const char *v = p + nl + 1; while (v < le && (*v == ' ' || *v == '\t')) v++;
            const char *ve = le; while (ve > v && (ve[-1] == '\r' || ve[-1] == ' ')) ve--;
            int o = 0; for (const char *q = v; q < ve && o < cap - 1; q++) out[o++] = *q; out[o] = 0; return 1;
        }
        p = (le < end) ? le + 1 : end;
    }
    return 0;
}
/* resolve `link` (absolute, root-relative or relative) against the current page -> absolute http URL */
static void web_abs(char *out, int cap, const char *link) {
    if (starts(link, "http://") || starts(link, "https://")) { strncpy(out, link, cap - 1); out[cap - 1] = 0; return; }
    int o = 0;
    for (const char *p = "http://"; *p && o < cap - 1; p++) out[o++] = *p;
    for (const char *p = web_host; *p && o < cap - 1; p++) out[o++] = *p;
    if (web_port != 80) { char pb[8]; snprintf(pb, sizeof pb, ":%d", web_port); for (char *q = pb; *q && o < cap - 1; q++) out[o++] = *q; }
    if (link[0] == '/') { for (const char *p = link; *p && o < cap - 1; p++) out[o++] = *p; }
    else {
        int cut = 0; for (int i = 0; web_path[i]; i++) if (web_path[i] == '/') cut = i + 1;
        for (int i = 0; i < cut && o < cap - 1; i++) out[o++] = web_path[i];
        for (const char *p = link; *p && o < cap - 1; p++) out[o++] = *p;
    }
    out[o] = 0;
}
/* load a URL: parse, fetch, follow http redirects, print status + rendered text */
static int web_load(const char *url) {
    char cur[WEB_URLM]; strncpy(cur, url, sizeof cur - 1); cur[sizeof cur - 1] = 0;
    for (int hop = 0; hop < 5; hop++) {
        if (web_parse(cur) != 0) { outs(2, "web: only http:// URLs are supported\n"); return -1; }
        out1("[fetching "); out1(web_host); out1(web_path); out1("]\n");
        if (web_fetch() < 0) { outs(2, "web: cannot fetch "); outs(2, web_host); outs(2, "\n"); return -1; }
        char *hdr_end = strstr(web_buf, "\r\n\r\n");
        int code = 0;
        if (starts(web_buf, "http/")) { const char *sp = web_buf; while (*sp && *sp != ' ') sp++; code = atoi(sp); }
        if (hdr_end) {
            const char *nl = strchr(web_buf, '\n');       /* echo the status line */
            if (nl) { int sl = (int)(nl - web_buf); if (sl > 0 && web_buf[sl - 1] == '\r') sl--; write(1, web_buf, sl); write(1, "\n", 1); }
            if (code >= 300 && code < 400) {
                char loc[WEB_URLM];
                if (web_header(web_buf, hdr_end, "location", loc, sizeof loc) && loc[0]) {
                    if (starts(loc, "https://")) { outs(2, "web: redirect to https (TLS unsupported): "); outs(2, loc); outs(2, "\n"); return -1; }
                    char nx[WEB_URLM]; web_abs(nx, sizeof nx, loc);
                    strncpy(cur, nx, sizeof cur - 1); cur[sizeof cur - 1] = 0;
                    continue;
                }
            }
            web_render(hdr_end + 4);
        } else web_render(web_buf);
        return 0;
    }
    outs(2, "web: too many redirects\n");
    return -1;
}
static int ap_web(int argc, char **argv) {
    if (argc < 2) { outs(2, "usage: web <url>   (http only, e.g. web example.com  or  web http://10.0.2.2/)\n"); return 1; }
    char url[WEB_URLM]; strncpy(url, argv[1], sizeof url - 1); url[sizeof url - 1] = 0;
    for (;;) {
        out1("\n");
        if (web_load(url) == 0 && web_nlink > 0) {
            out1("\n---- links ----\n");
            for (int i = 0; i < web_nlink; i++) { char n[16]; snprintf(n, sizeof n, "[%d] ", i + 1); out1(n); out1(web_links[i]); write(1, "\n", 1); }
        }
        out1("\n[web] number=open link, u <url>=go, r=reload, q=quit > ");
        char line[WEB_URLM]; int li = 0;
        for (;;) { char ch; if (read(0, &ch, 1) <= 0) { write(1, "\n", 1); return 0; } if (ch == '\n') break; if (li < (int)sizeof line - 1) line[li++] = ch; }
        line[li] = 0;
        char *s = line; while (*s == ' ') s++;
        char *e = s + strlen(s); while (e > s && (e[-1] == ' ' || e[-1] == '\r')) *--e = 0;
        if (!*s) continue;
        if (!strcmp(s, "q")) break;
        if (!strcmp(s, "r")) continue;
        if ((s[0] == 'u' || s[0] == 'g') && s[1] == ' ') { char *u = s + 2; while (*u == ' ') u++; strncpy(url, u, sizeof url - 1); url[sizeof url - 1] = 0; continue; }
        int isnum = 1; for (char *t = s; *t; t++) if (*t < '0' || *t > '9') { isnum = 0; break; }
        if (isnum) {
            int idx = atoi(s);
            if (idx >= 1 && idx <= web_nlink) { char nx[WEB_URLM]; web_abs(nx, sizeof nx, web_links[idx - 1]); strncpy(url, nx, sizeof url - 1); url[sizeof url - 1] = 0; }
            else out1("no such link\n");
            continue;
        }
        strncpy(url, s, sizeof url - 1); url[sizeof url - 1] = 0;
    }
    return 0;
}

/* ---- dispatch ------------------------------------------------------------ */
static const struct { const char *name; int (*fn)(int, char **); } g_applets[] = {
    { "sh", run_shell }, { "echo", ap_echo }, { "cat", ap_cat }, { "more", ap_more }, { "grep", ap_grep }, { "sed", ap_sed }, { "awk", ap_awk },
    { "wc", ap_wc }, { "head", ap_head }, { "tail", ap_tail }, { "rev", ap_rev },
    { "tee", ap_tee }, { "nl", ap_nl }, { "yes", ap_yes }, { "seq", ap_seq },
    { "sort", ap_sort }, { "uniq", ap_uniq }, { "tr", ap_tr }, { "ls", ap_ls }, { "tree", ap_tree },
    { "cp", ap_cp }, { "mv", ap_mv }, { "rm", ap_rm }, { "mkdir", ap_mkdir },
    { "rmdir", ap_rmdir }, { "touch", ap_touch }, { "cmp", ap_cmp }, { "stat", ap_stat },
    { "basename", ap_basename }, { "dirname", ap_dirname }, { "pwd", ap_pwd },
    { "uname", ap_uname }, { "sleep", ap_sleep }, { "date", ap_date }, { "id", ap_id },
    { "whoami", ap_whoami }, { "hostname", ap_hostname }, { "env", ap_env },
    { "clear", ap_clear }, { "free", ap_free }, { "sync", ap_sync }, { "true", ap_true },
    { "false", ap_false }, { "kill", ap_kill }, { "test", ap_test }, { "[", ap_test },
    { "expr", ap_expr }, { "vi", ap_vi }, { "ping", ap_ping }, { "web", ap_web },
    { "ifconfig", ap_ifconfig }, { "help", ap_help },
};
#define NAPPLET ((int)(sizeof g_applets / sizeof g_applets[0]))

static int (*applet_fn(const char *name))(int, char **) {
    for (int i = 0; i < NAPPLET; i++) if (!strcmp(g_applets[i].name, name)) return g_applets[i].fn;
    return 0;
}
static int run_applet(const char *name, int argc, char **argv) {
    int (*fn)(int, char **) = applet_fn(name);
    if (fn) return fn(argc, argv);
    outs(2, name); outs(2, ": applet not found\n");
    return 127;
}
static int ap_help(int argc, char **argv) {
    (void)argc; (void)argv;
    out1("mbb applets:");
    for (int i = 0; i < NAPPLET; i++) { write(1, (i % 10) ? " " : "\n  ", (i % 10) ? 1 : 3); out1(g_applets[i].name); }
    write(1, "\n", 1);
    return 0;
}

/* Locate and exec a command.  A name containing '/' is taken as a literal path
 * (/abs/foo or ./rel/foo).  A bare name is resolved as a "command on the path"
 * WITHOUT needing ./ or a cwd match: TinyOS ships programs as uppercase 8.3
 * files at the root (/GIT.BIN, /WGET.BIN, ...) and FAT lookup is case-
 * insensitive, so `git` -> /git.bin -> /GIT.BIN works from any directory.
 * Returns only on failure (control returns iff no exec succeeded). */
static void exec_command(char **argv) {
    const char *cmd = argv[0];
    if (strchr(cmd, '/')) { execv(cmd, argv); return; }         /* explicit path */
    char cand[128];
    snprintf(cand, sizeof cand, "/%s", cmd);         execv(cand, argv);  /* /git       */
    snprintf(cand, sizeof cand, "/%s.bin", cmd);     execv(cand, argv);  /* /GIT.BIN   */
    snprintf(cand, sizeof cand, "/bin/%s", cmd);     execv(cand, argv);  /* /bin/git   */
    snprintf(cand, sizeof cand, "/bin/%s.bin", cmd); execv(cand, argv);  /* /bin/GIT.BIN */
    execvp(cmd, argv);                                          /* PATH fallback */
}

/* ---- pipeline + shell ---------------------------------------------------- */
struct shell_stage {
    char *argv[24];
    unsigned char glob[24];
    char text[512];
    const char *input;
    const char *output;
    int argc;
    int append;
};

static int parse_stage(const char *source, struct shell_stage *stage) {
    memset(stage, 0, sizeof *stage);
    char *writep = stage->text;
    char *end = stage->text + sizeof stage->text;
    int redirect = 0;

    while (*source) {
        while (*source == ' ') source++;
        if (!*source) break;
        if (*source == '<' || *source == '>') {
            redirect = *source++;
            if (redirect == '>') {
                stage->append = *source == '>';
                if (stage->append) source++;
            }
            while (*source == ' ') source++;
            if (!*source || *source == '<' || *source == '>') {
                outs(2, "sh: missing redirection file\n");
                return -1;
            }
        }

        char *word = writep;
        int single = 0, dbl = 0, allow_glob = 0;
        while (*source) {
            if (!dbl && *source == '\'') { single = !single; source++; continue; }
            if (!single && *source == '"') { dbl = !dbl; source++; continue; }
            if (!single && *source == '\\' && source[1]) {
                source++;
                if (writep + 1 >= end) { outs(2, "sh: command too long\n"); return -1; }
                *writep++ = *source++;
                continue;
            }
            if (!single && !dbl && (*source == ' ' || *source == '<' || *source == '>')) break;
            if (writep + 1 >= end) { outs(2, "sh: command too long\n"); return -1; }
            if (!single && !dbl && (*source == '*' || *source == '?' || *source == '[')) allow_glob = 1;
            *writep++ = *source++;
        }
        if (single || dbl) { outs(2, "sh: unterminated quote\n"); return -1; }
        if (writep == word) continue;
        *writep++ = 0;
        if (redirect == '<') stage->input = word;
        else if (redirect == '>') stage->output = word;
        else if (stage->argc < 23) {
            stage->glob[stage->argc] = (unsigned char)allow_glob;
            stage->argv[stage->argc++] = word;
        }
        else { outs(2, "sh: too many arguments\n"); return -1; }
        redirect = 0;
    }
    stage->argv[stage->argc] = 0;
    if (redirect) { outs(2, "sh: missing redirection file\n"); return -1; }
    if (!stage->argc) { outs(2, "sh: empty pipeline stage\n"); return -1; }
    return 0;
}

static int expand_stage_globs(struct shell_stage *stage, char **argv, int capacity) {
    int argc = 0;
    for (int i = 0; i < stage->argc; i++) {
        tglob_result matches;
        int count = stage->glob[i] ? tglob_expand(stage->argv[i], &matches) : 0;
        if (count < 0) { outs(2, "sh: glob: out of memory\n"); return -1; }
        if (count == 0) {
            if (argc + 1 >= capacity) { outs(2, "sh: too many expanded arguments\n"); return -1; }
            argv[argc++] = stage->argv[i];
        } else {
            for (int j = 0; j < count; j++) {
                if (argc + 1 >= capacity) { tglob_free(&matches); outs(2, "sh: too many expanded arguments\n"); return -1; }
                argv[argc++] = strdup(matches.items[j]);
                if (!argv[argc - 1]) { tglob_free(&matches); return -1; }
            }
            tglob_free(&matches);
        }
    }
    argv[argc] = 0;
    return argc;
}

static int run_pipeline(char *cmd) {
    char *stages[8]; int ns = 0;
    stages[ns++] = cmd;
    for (char *p = cmd; *p && ns < 8; p++) if (*p == '|') { *p = 0; stages[ns++] = p + 1; }

    struct shell_stage parsed[8];
    for (int s = 0; s < ns; s++) if (parse_stage(stages[s], &parsed[s]) != 0) return 2;

    int prev_read = -1, pids[8], np = 0;
    for (int s = 0; s < ns; s++) {
        struct shell_stage *stage = &parsed[s];

        int pfd[2] = { -1, -1 };
        if (s < ns - 1) pipe(pfd);
        int pid = fork();
        if (pid == 0) {
            char *argv[128];
            int argc = expand_stage_globs(stage, argv, 128);
            if (argc < 0) _exit(1);
            signal(SIGINT, SIG_DFL);            /* foreground child dies on Ctrl-C */
            if (prev_read >= 0) { dup2(prev_read, 0); close(prev_read); }
            if (pfd[1] >= 0) { dup2(pfd[1], 1); close(pfd[1]); close(pfd[0]); }
            if (stage->input) {
                int fd = open(stage->input, O_RDONLY);
                if (fd < 0) { outs(2, "sh: cannot open "); outs(2, stage->input); outs(2, "\n"); _exit(1); }
                dup2(fd, 0); close(fd);
            }
            if (stage->output) {
                int flags = O_WRONLY | O_CREAT | (stage->append ? O_APPEND : O_TRUNC);
                int fd = open(stage->output, flags, 0666);
                if (fd < 0) { outs(2, "sh: cannot open "); outs(2, stage->output); outs(2, "\n"); _exit(1); }
                dup2(fd, 1); close(fd);
            }
            if (applet_fn(argv[0])) _exit(run_applet(argv[0], argc, argv));
            exec_command(argv);                 /* /abs, ./rel, or a bare name -> /NAME.BIN on the path */
            outs(2, argv[0]); outs(2, ": not found\n");
            _exit(127);
        }
        pids[np++] = pid;
        if (prev_read >= 0) close(prev_read);
        if (pfd[1] >= 0) close(pfd[1]);
        prev_read = pfd[0];
    }
    if (prev_read >= 0) close(prev_read);

    int result = 0;
    for (int i = 0; i < np; i++) { int st, w = wait(&st); if (w == pids[np - 1] && WIFEXITED(st)) result = WEXITSTATUS(st); }
    return result;
}

static int shell_io_selftest(void) {
    static char write_file[] = "echo hello > /mbbio.txt";
    static char append_file[] = "echo world >> /mbbio.txt";
    static char input_file[] = "wc -w < /mbbio.txt > /mbbwc.txt";
    static char pipe_file[] = "echo pipeline | wc -w > /mbbpipe.txt";
    int ok = run_pipeline(write_file) == 0 && run_pipeline(append_file) == 0 &&
             run_pipeline(input_file) == 0 && run_pipeline(pipe_file) == 0;
    char buf[32];
    int fd = ok ? open("/mbbio.txt", O_RDONLY) : -1;
    int n = fd >= 0 ? read(fd, buf, sizeof buf) : -1;
    if (fd >= 0) close(fd);
    if (n != 12 || memcmp(buf, "hello\nworld\n", 12)) ok = 0;
    fd = ok ? open("/mbbwc.txt", O_RDONLY) : -1;
    n = fd >= 0 ? read(fd, buf, sizeof buf) : -1;
    if (fd >= 0) close(fd);
    if (n <= 0 || !memchr(buf, '2', n)) ok = 0;
    fd = ok ? open("/mbbpipe.txt", O_RDONLY) : -1;
    n = fd >= 0 ? read(fd, buf, sizeof buf) : -1;
    if (fd >= 0) close(fd);
    if (n <= 0 || !memchr(buf, '1', n)) ok = 0;
    unlink("/mbbio.txt"); unlink("/mbbwc.txt"); unlink("/mbbpipe.txt");
    return ok ? 0 : -1;
}

/* Run a line: split on ';', handle shell built-ins (cd/exit), else pipeline. */
static int run_line(char *line) {
    int rc = 0;
    char *seg = line;
    for (char *p = line; ; p++) {
        if (*p == ';' || *p == 0) {
            int end = (*p == 0); *p = 0;
            const char *q = seg; while (*q == ' ') q++;
            char first[32]; int fi = 0; while (*q && *q != ' ' && fi < 31) first[fi++] = *q++;
            first[fi] = 0;
            if (!strcmp(first, "cd")) { while (*q == ' ') q++; if (chdir(*q ? q : "/") < 0) outs(2, "cd: no such directory\n"); }
            else if (!strcmp(first, "exit")) return -1;
            else if (fi) rc = run_pipeline(seg);
            if (end) break;
            seg = p + 1;
        }
    }
    return rc;
}

static int run_shell(int argc, char **argv) {
    signal(SIGINT, SIG_IGN);                    /* the shell survives Ctrl-C */
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "-c") && i + 1 < argc) {
            static char buf[512]; int j = 0;
            for (const char *p = argv[i + 1]; *p && j < 511; p++) buf[j++] = *p;
            buf[j] = 0;
            int r = run_line(buf);
            return r < 0 ? 0 : r;
        }
    char line[512], cwd[256];
    for (;;) {
        if (getcwd(cwd, sizeof cwd)) out1(cwd);
        out1(" $ ");
        int li = 0; char c;
        for (;;) { int r = read(0, &c, 1); if (r <= 0) { write(1, "\n", 1); return 0; } if (c == '\n') break; if (li < 511) line[li++] = c; }
        line[li] = 0;
        if (li && run_line(line) < 0) { out1("bye\n"); return 0; }
    }
}

static const char *base(const char *p) { const char *b = p; for (const char *q = p; *q; q++) if (*q == '/') b = q + 1; return b; }

/* Headless TinyGL check: open a zero-copy buffer object, draw into the mapped
 * pixels, and read them back through the mapping (proving the app writes land in
 * the kernel BO with no copy).  Does not present, so it never disturbs the boot
 * display.  0 = ok. */
static int gfx_selftest(void) {
    tgl_t g;
    if (tgl_open(&g, 64, 48) != 0) return -1;
    tgl_clear(&g, 0x123456);
    tgl_pixel(&g, 10, 10, 0xABCDEF);
    uint32_t want_bg = tgl_rgb(&g, 0x123456), want_px = tgl_rgb(&g, 0xABCDEF);
    int ok = g.px && g.px[0] == want_bg && g.px[10 * g.stride + 10] == want_px;
    tgl_finish(&g);
    return ok ? 0 : -2;
}

/* Headless 3D check: rasterize into a mapped BO offscreen and confirm the depth
 * test and back-face cull select the right fragments (the G4 gate, user-side). */
static int gfx3d_selftest(void) {
    tgl_t g;
    if (tgl_open(&g, 64, 64) != 0) return -1;
    tgl3d_t r;
    if (tgl3d_begin(&r, &g) != 0) { tgl_finish(&g); return -1; }
    uint32_t rp = tgl_rgb(&g, 0xFF0000), gp = tgl_rgb(&g, 0x00FF00);
    tgl_tex rtex = { &rp, 1, 1 }, gtex = { &gp, 1, 1 };
    tgl_vert na = {2,2,TGL_FXONE,0,0}, nb = {61,2,TGL_FXONE,0,0}, nc = {30,61,TGL_FXONE,0,0};
    tgl_vert fa = {2,2,TGL_FXONE/2,0,0}, fb2 = {61,2,TGL_FXONE/2,0,0}, fc = {30,61,TGL_FXONE/2,0,0};
    int ci = 20 * g.stride + 30;

    tgl_clear(&g, 0x000000); tgl3d_clear_depth(&r); tgl3d_cull(&r, 0);
    tgl3d_tri(&r, &na, &nb, &nc, &rtex, 256);                /* near red */
    int ok = g.px[ci] == rp;
    tgl3d_tri(&r, &fa, &fb2, &fc, &gtex, 256);               /* far green loses depth */
    ok = ok && g.px[ci] == rp;

    tgl_clear(&g, 0x000000); tgl3d_clear_depth(&r);
    tgl3d_tri(&r, &fa, &fb2, &fc, &gtex, 256);               /* far first */
    tgl3d_tri(&r, &na, &nb, &nc, &rtex, 256);                /* near wins regardless of order */
    ok = ok && g.px[ci] == rp;

    tgl_clear(&g, 0x000000); tgl3d_clear_depth(&r); tgl3d_cull(&r, 1);
    tgl3d_tri(&r, &nc, &nb, &na, &rtex, 256);                /* reversed winding -> culled */
    ok = ok && g.px[ci] == 0u;
    tgl3d_tri(&r, &na, &nb, &nc, &rtex, 256);                /* front-facing -> drawn */
    ok = ok && g.px[ci] == rp;

    tgl3d_end(&r); tgl_finish(&g);
    return ok ? 0 : -2;
}

/* Headless socket check: a local server and client over the kernel TCP loopback
 * exchange bytes both ways using the BSD socket syscalls (the N3 gate). */
static int net_sock_selftest(void) {
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(9100);
    a.sin_addr.s_addr = htonl(0x0A00020Fu);          /* 10.0.2.15 -> kernel loopback */
    if (bind(ls, (struct sockaddr *)&a, sizeof a) != 0) return -2;
    if (listen(ls, 1) != 0) return -3;
    int cs = socket(AF_INET, SOCK_STREAM, 0);
    if (cs < 0) return -4;
    if (connect(cs, (struct sockaddr *)&a, sizeof a) != 0) return -5;
    int as = accept(ls, 0, 0);
    if (as < 0) return -6;
    if (write(cs, "ping", 4) != 4) return -7;
    char b[16];
    int n = read(as, b, sizeof b);
    if (n != 4 || memcmp(b, "ping", 4)) return -8;
    if (write(as, "pong!", 5) != 5) return -9;
    n = read(cs, b, sizeof b);
    if (n != 5 || memcmp(b, "pong!", 5)) return -10;
    close(cs); close(as); close(ls);
    return 0;
}

static int cp_selftest(void) {
    int ok = mkdir("/cpt", 0755) == 0 && mkdir("/cpt/s", 0755) == 0 &&
             mkdir("/cpt/s/d", 0755) == 0;
    int fd = ok ? open("/cpt/s/a", O_WRONLY | O_CREAT | O_TRUNC, 0666) : -1;
    if (fd < 0 || write(fd, "alpha", 5) != 5) ok = 0;
    if (fd >= 0) close(fd);
    fd = ok ? open("/cpt/s/d/b", O_WRONLY | O_CREAT | O_TRUNC, 0666) : -1;
    if (fd < 0 || write(fd, "beta", 4) != 4) ok = 0;
    if (fd >= 0) close(fd);
    if (ok && cp_tree("/cpt/s", "/cpt/o", 1) != 0) ok = 0;
    char buf[8];
    fd = ok ? open("/cpt/o/a", O_RDONLY) : -1;
    if (fd < 0 || read(fd, buf, sizeof buf) != 5 || memcmp(buf, "alpha", 5)) ok = 0;
    if (fd >= 0) close(fd);
    fd = ok ? open("/cpt/o/d/b", O_RDONLY) : -1;
    if (fd < 0 || read(fd, buf, sizeof buf) != 4 || memcmp(buf, "beta", 4)) ok = 0;
    if (fd >= 0) close(fd);
    unlink("/cpt/o/d/b"); rmdir("/cpt/o/d"); unlink("/cpt/o/a"); rmdir("/cpt/o");
    unlink("/cpt/s/d/b"); rmdir("/cpt/s/d"); unlink("/cpt/s/a"); rmdir("/cpt/s"); rmdir("/cpt");
    return ok ? 0 : -1;
}

int main(int argc, char **argv) {
    const char *ap = base(argv[0]);
    if (applet_fn(ap)) return run_applet(ap, argc, argv);        /* invoked by applet name */
    if (argc < 2) {                                             /* self-test: real pipelines */
        static char a[] = "echo hello | grep hello";
        static char b[] = "seq 1 3 | wc -l | grep 3";
        int ok = (run_pipeline(a) == 0) && (run_pipeline(b) == 0);
        struct stat st;                                         /* legacy fs syscalls: mkdir/stat/rmdir */
        if (mkdir("/mbbself", 0755) != 0) ok = 0;
        else {
            struct stat rel;                                    /* relative "./" must resolve too */
            if (stat("/mbbself", &st) != 0 || !S_ISDIR(st.st_mode)) ok = 0;
            if (stat("./mbbself", &rel) != 0 || !S_ISDIR(rel.st_mode)) ok = 0;
            rmdir("/mbbself");
        }
        static char x[] = "/MBB.BIN true";                      /* shell must exec external ELFs */
        if (run_pipeline(x) != 0) ok = 0;
        if (time(0) < 1500000000L) ok = 0;                      /* wall clock seeded from UEFI GetTime */
        if (mkdir("/xt", 0755) == 0) {                          /* exec via a ".." relative path */
            if (chdir("/xt") == 0) {
                static char r[] = "../MBB.BIN true";
                if (run_pipeline(r) != 0) ok = 0;
                chdir("/");
            }
            rmdir("/xt");
        }
        outs(1, gfx_selftest() == 0 ? "[tinygl-umd] BO map + draw + zero-copy readback ok\n"
                                    : "[tinygl-umd] BO self-test FAILED\n");
        outs(1, gfx3d_selftest() == 0 ? "[tinygl-umd] 3D raster + depth + cull ok\n"
                                      : "[tinygl-umd] 3D self-test FAILED\n");
        outs(1, net_sock_selftest() == 0 ? "[net-umd] socket loopback connect+data+close ok\n"
                                         : "[net-umd] socket self-test FAILED\n");
        int cp_result = cp_selftest();
        outs(1, cp_result == 0 ? "[mbb-cp] recursive directory copy ok\n"
                       : "[mbb-cp] recursive directory copy FAILED\n");
        if (cp_result != 0) ok = 0;
        int shell_io_result = shell_io_selftest();
        outs(1, shell_io_result == 0 ? "[mbb-sh] pipe + redirection operators ok\n"
                         : "[mbb-sh] pipe + redirection operators FAILED\n");
        if (shell_io_result != 0) ok = 0;
        return ok ? 0 : 1;
    }
    (void)ci_eq;
    return run_applet(argv[1], argc - 1, argv + 1);             /* multi-call: mbb APPLET ... */
}
