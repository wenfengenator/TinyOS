/* SPDX-License-Identifier: GPL-2.0-only */

/* TinyOS userspace git plumbing — Cycle 9 C6b/C6c (git over HTTPS).
 *
 * The transport-independent half of a git client: the smart-HTTP pkt-line
 * framing, the packfile format (RFC-less but documented in git's
 * Documentation/gitformat-pack.txt), delta reconstruction, and just enough
 * commit/tree parsing to check a working tree out.  The actual HTTPS GET/POST
 * lives in git.c; everything here is pure computation over byte buffers so it
 * can be unit-tested on the host against packfiles produced by real git.
 *
 * Objects are held in a malloc'd store (the kernel provides brk/mmap), so this
 * targets small-to-medium repositories.  Depends on tcrypto.h (SHA-1 object
 * ids) and tinflate.h (zlib inflate).  Plain C for on-device TinyCC. */
#ifndef TGIT_H
#define TGIT_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include "tcrypto.h"
#include "tinflate.h"

/* ------------------------------------------------------------------ */
/* small helpers                                                      */
/* ------------------------------------------------------------------ */
static int git_hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Parse exactly 4 hex digits (pkt-line length prefix); -1 on error. */
static long git_hex4(const uint8_t *p) {
    long v = 0;
    for (int i = 0; i < 4; i++) {
        int d = git_hexval(p[i]);
        if (d < 0) return -1;
        v = (v << 4) | d;
    }
    return v;
}

static uint32_t git_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* 20-byte binary sha -> 40-char lowercase hex (caller supplies 41-byte buf). */
static void git_sha_hex(const uint8_t sha[20], char out[41]) {
    static const char h[] = "0123456789abcdef";
    for (int i = 0; i < 20; i++) { out[i * 2] = h[sha[i] >> 4]; out[i * 2 + 1] = h[sha[i] & 15]; }
    out[40] = 0;
}

/* 40-char hex -> 20-byte binary sha; 0 on success. */
static int git_hex_sha(const char *hex, uint8_t sha[20]) {
    for (int i = 0; i < 20; i++) {
        int hi = git_hexval((unsigned char)hex[i * 2]);
        int lo = git_hexval((unsigned char)hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        sha[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* pkt-line framing (smart HTTP)                                      */
/* ------------------------------------------------------------------ */
/* Read the next pkt-line from buf[*p..len).  Returns the payload length and
 * points *payload at the data; 0 with *payload==NULL for a flush-pkt (0000);
 * -1 on a malformed frame or truncation. */
static long git_pkt_next(const uint8_t *buf, size_t len, size_t *p, const uint8_t **payload) {
    if (*p + 4 > len) return -1;
    long n = git_hex4(buf + *p);
    if (n < 0) return -1;
    if (n == 0) { *p += 4; *payload = NULL; return 0; }   /* flush-pkt */
    if (n < 4 || *p + (size_t)n > len) return -1;
    *payload = buf + *p + 4;
    *p += n;
    return n - 4;
}

/* Format a pkt-line ("%04x" length + payload) into out; returns bytes written. */
static size_t git_pkt_put(uint8_t *out, const char *payload, size_t plen) {
    size_t total = plen + 4;
    static const char h[] = "0123456789abcdef";
    out[0] = h[(total >> 12) & 15]; out[1] = h[(total >> 8) & 15];
    out[2] = h[(total >> 4) & 15];  out[3] = h[total & 15];
    memcpy(out + 4, payload, plen);
    return total;
}

/* ------------------------------------------------------------------ */
/* packfile objects                                                   */
/* ------------------------------------------------------------------ */
enum { GIT_COMMIT = 1, GIT_TREE = 2, GIT_BLOB = 3, GIT_TAG = 4,
       GIT_OFS_DELTA = 6, GIT_REF_DELTA = 7 };

typedef struct {
    uint64_t offset;        /* file offset of this object within the pack */
    int      raw_type;      /* type as encoded (may be OFS/REF delta) */
    int      type;          /* resolved git type (commit/tree/blob/tag) */
    uint8_t *data;          /* resolved content (owned) */
    size_t   len;
    uint64_t base_off;      /* OFS_DELTA: absolute pack offset of the base */
    uint8_t  base_sha[20];  /* REF_DELTA: object id of the base */
    uint8_t *delta;         /* raw (inflated) delta program, before resolution */
    size_t   dlen;
    uint8_t  sha[20];       /* git object id, valid once resolved */
    int      resolved;
} gobj;

typedef struct { gobj *objs; int n; } gpack;

/* git-style base-128 varints come in three flavours; keep them separate. */

/* delta header/size varint: LSB-first, 7 bits/byte, 0x80 = continue. */
static uint64_t git_dvarint(const uint8_t *d, size_t n, size_t *p) {
    uint64_t v = 0; int shift = 0; uint8_t c;
    do {
        if (*p >= n) return v;
        c = d[(*p)++];
        v |= (uint64_t)(c & 0x7f) << shift;
        shift += 7;
    } while (c & 0x80);
    return v;
}

/* pack object header: first byte carries type in bits 6-4 and the low 4 size
 * bits; further bytes add 7 size bits each (LSB-first). */
static int git_obj_header(const uint8_t *d, size_t n, size_t *p, uint64_t *size) {
    if (*p >= n) return -1;
    uint8_t c = d[(*p)++];
    int type = (c >> 4) & 7;
    uint64_t sz = c & 0x0f;
    int shift = 4;
    while (c & 0x80) {
        if (*p >= n) return -1;
        c = d[(*p)++];
        sz |= (uint64_t)(c & 0x7f) << shift;
        shift += 7;
    }
    *size = sz;
    return type;
}

/* OFS_DELTA base distance: big-endian with the +1 bias git uses. */
static uint64_t git_ofs_varint(const uint8_t *d, size_t n, size_t *p) {
    if (*p >= n) return 0;
    uint8_t c = d[(*p)++];
    uint64_t off = c & 0x7f;
    while (c & 0x80) {
        if (*p >= n) break;
        c = d[(*p)++];
        off = ((off + 1) << 7) | (c & 0x7f);
    }
    return off;
}

/* Reconstruct a delta target from its base.  Returns length or -1. */
static long git_apply_delta(const uint8_t *base, size_t blen,
                            const uint8_t *d, size_t dlen, uint8_t **out) {
    size_t p = 0;
    (void)git_dvarint(d, dlen, &p);             /* source size (unused, sanity only) */
    uint64_t dst = git_dvarint(d, dlen, &p);    /* result size */
    uint8_t *o = (uint8_t *)malloc(dst ? dst : 1);
    if (!o) return -1;
    size_t op = 0;
    while (p < dlen) {
        uint8_t c = d[p++];
        if (c & 0x80) {                         /* copy from base */
            uint64_t coff = 0, csize = 0;
            if (c & 0x01) coff  |= (uint64_t)d[p++];
            if (c & 0x02) coff  |= (uint64_t)d[p++] << 8;
            if (c & 0x04) coff  |= (uint64_t)d[p++] << 16;
            if (c & 0x08) coff  |= (uint64_t)d[p++] << 24;
            if (c & 0x10) csize |= (uint64_t)d[p++];
            if (c & 0x20) csize |= (uint64_t)d[p++] << 8;
            if (c & 0x40) csize |= (uint64_t)d[p++] << 16;
            if (csize == 0) csize = 0x10000;
            if (coff + csize > blen || op + csize > dst) { free(o); return -1; }
            memcpy(o + op, base + coff, csize); op += csize;
        } else if (c) {                         /* insert literal bytes */
            if (op + c > dst || p + c > dlen) { free(o); return -1; }
            memcpy(o + op, d + p, c); p += c; op += c;
        } else { free(o); return -1; }          /* 0x00 opcode is invalid */
    }
    if (op != dst) { free(o); return -1; }
    *out = o;
    return (long)dst;
}

static const char *git_typestr(int t) {
    switch (t) {
        case GIT_COMMIT: return "commit";
        case GIT_TREE:   return "tree";
        case GIT_BLOB:   return "blob";
        case GIT_TAG:    return "tag";
        default:         return "?";
    }
}

/* git object id = SHA-1 of "<type> <len>\0<content>". */
static void git_oid(int type, const uint8_t *data, size_t len, uint8_t sha[20]) {
    char hdr[32];
    int hn = 0;
    const char *ts = git_typestr(type);
    while (*ts) hdr[hn++] = *ts++;
    hdr[hn++] = ' ';
    char num[24]; int nn = 0;
    size_t v = len;
    if (v == 0) num[nn++] = '0';
    while (v) { num[nn++] = '0' + (v % 10); v /= 10; }
    while (nn) hdr[hn++] = num[--nn];
    hdr[hn++] = 0;
    sha1_ctx c; sha1_init(&c);
    sha1_update(&c, hdr, hn);
    sha1_update(&c, data, len);
    sha1_final(&c, sha);
}

static gobj *git_by_offset(gpack *gp, uint64_t off) {
    for (int i = 0; i < gp->n; i++) if (gp->objs[i].offset == off) return &gp->objs[i];
    return NULL;
}

static gobj *git_find(gpack *gp, const uint8_t sha[20]) {
    for (int i = 0; i < gp->n; i++)
        if (gp->objs[i].resolved && memcmp(gp->objs[i].sha, sha, 20) == 0) return &gp->objs[i];
    return NULL;
}

/* Recursively resolve one object (following delta chains) and hash it. */
static int git_resolve(gpack *gp, gobj *o) {
    if (o->resolved) return 0;
    if (o->raw_type >= GIT_COMMIT && o->raw_type <= GIT_TAG) {
        o->type = o->raw_type;
    } else {
        gobj *base = (o->raw_type == GIT_OFS_DELTA)
                        ? git_by_offset(gp, o->base_off)
                        : git_find(gp, o->base_sha);
        if (!base) return -1;
        if (git_resolve(gp, base) != 0) return -1;
        uint8_t *res = NULL;
        long rl = git_apply_delta(base->data, base->len, o->delta, o->dlen, &res);
        if (rl < 0) return -1;
        free(o->delta); o->delta = NULL;
        o->data = res; o->len = (size_t)rl; o->type = base->type;
    }
    git_oid(o->type, o->data, o->len, o->sha);
    o->resolved = 1;
    return 0;
}

/* Parse a whole packfile and resolve every object.  Returns 0 on success and
 * fills *gp (caller frees with git_free_pack).  Verifies the trailer SHA-1. */
static int git_parse_pack(const uint8_t *pack, size_t plen, gpack *gp) {
    gp->objs = NULL; gp->n = 0;
    if (plen < 32 || memcmp(pack, "PACK", 4) != 0) return -1;
    if (git_be32(pack + 4) != 2) return -1;
    uint32_t nobj = git_be32(pack + 8);
    if (nobj == 0 || nobj > 5000000) return -1;

    uint8_t trailer[20];
    sha1(pack, plen - 20, trailer);
    if (memcmp(trailer, pack + plen - 20, 20) != 0) return -1;

    gp->objs = (gobj *)calloc(nobj, sizeof(gobj));
    if (!gp->objs) return -1;
    gp->n = (int)nobj;

    size_t p = 12;
    for (uint32_t i = 0; i < nobj; i++) {
        gobj *o = &gp->objs[i];
        o->offset = p;
        uint64_t size = 0;
        int type = git_obj_header(pack, plen - 20, &p, &size);
        if (type < 0) return -1;
        o->raw_type = type;

        if (type == GIT_OFS_DELTA) {
            uint64_t back = git_ofs_varint(pack, plen - 20, &p);
            if (back == 0 || back > o->offset) return -1;
            o->base_off = o->offset - back;
        } else if (type == GIT_REF_DELTA) {
            if (p + 20 > plen - 20) return -1;
            memcpy(o->base_sha, pack + p, 20); p += 20;
        }

        uint8_t *mem = (uint8_t *)malloc(size ? size : 1);
        if (!mem) return -1;
        size_t used = 0;
        long got = tinfl_zlib(pack + p, (plen - 20) - p, mem, size, &used);
        if (got < 0 || (uint64_t)got != size) { free(mem); return -1; }
        p += used;

        if (type == GIT_OFS_DELTA || type == GIT_REF_DELTA) {
            o->delta = mem; o->dlen = (size_t)got;
        } else {
            o->data = mem; o->len = (size_t)got;
        }
    }

    for (int i = 0; i < gp->n; i++)
        if (git_resolve(gp, &gp->objs[i]) != 0) return -1;
    return 0;
}

static void git_free_pack(gpack *gp) {
    if (!gp->objs) return;
    for (int i = 0; i < gp->n; i++) { free(gp->objs[i].data); free(gp->objs[i].delta); }
    free(gp->objs);
    gp->objs = NULL; gp->n = 0;
}

/* ------------------------------------------------------------------ */
/* commit / tree parsing (for checkout)                               */
/* ------------------------------------------------------------------ */
/* Extract the tree oid from a commit body ("tree <40 hex>\n" first line). */
static int git_commit_tree(const uint8_t *body, size_t len, uint8_t sha[20]) {
    if (len < 46 || memcmp(body, "tree ", 5) != 0) return -1;
    return git_hex_sha((const char *)body + 5, sha);
}

/* Tree entry iterator: each entry is "<octal mode> <name>\0<20-byte sha>". */
typedef struct { const uint8_t *p, *end; } gtree_iter;

static void git_tree_begin(gtree_iter *it, const uint8_t *body, size_t len) {
    it->p = body; it->end = body + len;
}

static int git_tree_next(gtree_iter *it, unsigned *mode, const char **name, const uint8_t **sha) {
    const uint8_t *p = it->p;
    if (p >= it->end) return 0;
    unsigned m = 0;
    while (p < it->end && *p != ' ') { m = m * 8 + (*p - '0'); p++; }
    if (p >= it->end) return 0;
    p++;                                        /* skip space */
    *name = (const char *)p;
    while (p < it->end && *p != 0) p++;
    if (p + 1 + 20 > it->end + 1) return 0;
    p++;                                        /* skip NUL */
    *sha = p; p += 20;
    *mode = m;
    it->p = p;
    return 1;
}

#endif /* TGIT_H */
