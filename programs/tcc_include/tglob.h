/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef TINYOS_TGLOB_H
#define TINYOS_TGLOB_H

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} tglob_result;

static int tglob_has_meta(const char *text) {
    for (; *text; text++) if (*text == '*' || *text == '?' || *text == '[') return 1;
    return 0;
}

static int tglob_class(const char **pattern, unsigned char value) {
    const char *p = *pattern + 1;
    int invert = *p == '!' || *p == '^';
    int matched = 0;
    if (invert) p++;
    if (*p == ']') { matched = value == ']'; p++; }
    while (*p && *p != ']') {
        unsigned char first = (unsigned char)*p++;
        if (*p == '-' && p[1] && p[1] != ']') {
            p++;
            unsigned char last = (unsigned char)*p++;
            if (value >= first && value <= last) matched = 1;
        } else if (value == first) matched = 1;
    }
    if (*p != ']') return -1;
    *pattern = p + 1;
    return invert ? !matched : matched;
}

static int tglob_match(const char *pattern, const char *text) {
    while (*pattern) {
        if (*pattern == '*') {
            while (*pattern == '*') pattern++;
            if (!*pattern) return 1;
            do { if (tglob_match(pattern, text)) return 1; } while (*text++);
            return 0;
        }
        if (!*text) return 0;
        if (*pattern == '?') { pattern++; text++; continue; }
        if (*pattern == '[') {
            unsigned char value = (unsigned char)*text++;
            int matched = tglob_class(&pattern, value);
            if (matched < 0) {
                if (value != '[') return 0;
                pattern++;
            } else if (!matched) return 0;
            continue;
        }
        if (*pattern++ != *text++) return 0;
    }
    return !*text;
}

static int tglob_add(tglob_result *result, const char *path) {
    if (result->count == result->capacity) {
        size_t capacity = result->capacity ? result->capacity * 2 : 16;
        char **items = realloc(result->items, capacity * sizeof *items);
        if (!items) return -1;
        result->items = items;
        result->capacity = capacity;
    }
    size_t length = strlen(path) + 1;
    result->items[result->count] = malloc(length);
    if (!result->items[result->count]) return -1;
    memcpy(result->items[result->count++], path, length);
    return 0;
}

static int tglob_join(char *out, size_t size, const char *prefix, const char *name) {
    size_t prefix_length = strlen(prefix), name_length = strlen(name);
    int slash = prefix_length && prefix[prefix_length - 1] != '/';
    if (prefix_length + (size_t)slash + name_length + 1 > size) return -1;
    memcpy(out, prefix, prefix_length);
    if (slash) out[prefix_length++] = '/';
    memcpy(out + prefix_length, name, name_length + 1);
    return 0;
}

static int tglob_walk(const char *prefix, const char *pattern, tglob_result *result) {
    while (*pattern == '/') pattern++;
    const char *slash = strchr(pattern, '/');
    size_t length = slash ? (size_t)(slash - pattern) : strlen(pattern);
    if (!length) return 0;
    char component[256];
    if (length >= sizeof component) return 0;
    memcpy(component, pattern, length); component[length] = 0;
    const char *rest = slash ? slash + 1 : 0;
    int last = !rest || !*rest;

    if (!tglob_has_meta(component)) {
        char path[512];
        if (tglob_join(path, sizeof path, prefix, component) != 0) return 0;
        if (!last) return tglob_walk(path, rest, result);
        struct stat status;
        return stat(path, &status) == 0 ? tglob_add(result, path) : 0;
    }

    const char *directory = *prefix ? prefix : ".";
    DIR *dir = opendir(directory);
    if (!dir) return 0;
    struct dirent *entry;
    int rc = 0;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (entry->d_name[0] == '.' && component[0] != '.') continue;
        if (!tglob_match(component, entry->d_name)) continue;
        char path[512];
        if (tglob_join(path, sizeof path, prefix, entry->d_name) != 0) continue;
        rc = last ? tglob_add(result, path) : tglob_walk(path, rest, result);
        if (rc < 0) break;
    }
    closedir(dir);
    return rc;
}

static int tglob_compare(const void *left, const void *right) {
    const char *const *a = left, *const *b = right;
    return strcmp(*a, *b);
}

static int tglob_expand(const char *pattern, tglob_result *result) {
    memset(result, 0, sizeof *result);
    const char *rest = pattern;
    char prefix[2] = "";
    if (*rest == '/') { prefix[0] = '/'; prefix[1] = 0; rest++; }
    int rc = tglob_walk(prefix, rest, result);
    if (rc < 0) return -1;
    if (result->count > 1) qsort(result->items, result->count, sizeof *result->items, tglob_compare);
    return (int)result->count;
}

static void tglob_free(tglob_result *result) {
    for (size_t i = 0; i < result->count; i++) free(result->items[i]);
    free(result->items);
    memset(result, 0, sizeof *result);
}

#endif
