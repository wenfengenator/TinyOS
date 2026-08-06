/* SPDX-License-Identifier: GPL-2.0-only */

/* TinyOS userspace DEFLATE/zlib compressor — Cycle 10 (native git write path).
 *
 * The read side (tinflate.h) let git *read* objects; to *write* objects and
 * packs git must compress.  This is a small fixed-Huffman DEFLATE encoder
 * (RFC 1951, BTYPE=01) with greedy LZ77 matching via hash chains, wrapped in a
 * zlib stream (RFC 1950) so the output round-trips through any inflate.
 *
 * Object ids hash the *uncompressed* content, so the compression ratio never
 * affects correctness — real git reads these objects and computes the same
 * SHA-1.  Verified on the host against zlib and tinflate.h.  Plain C for tcc. */
#ifndef TDEFLATE_H
#define TDEFLATE_H

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* same base/extra tables as the inflate side (RFC 1951) */
static const short tdfl_lenb[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
    67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const short tdfl_lene[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
    4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const short tdfl_distb[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513,
    769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const short tdfl_diste[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
    9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

typedef struct { uint8_t *out; size_t cap, len; uint32_t acc; int nbits; int err; } tdfl_bw;

static void tdfl_bits(tdfl_bw *w, uint32_t bits, int n) {
    w->acc |= bits << w->nbits; w->nbits += n;
    while (w->nbits >= 8) {
        if (w->len >= w->cap) { w->err = 1; return; }
        w->out[w->len++] = (uint8_t)(w->acc & 0xff);
        w->acc >>= 8; w->nbits -= 8;
    }
}
static void tdfl_flush(tdfl_bw *w) {
    if (w->nbits > 0) {
        if (w->len >= w->cap) { w->err = 1; return; }
        w->out[w->len++] = (uint8_t)(w->acc & 0xff);
        w->acc = 0; w->nbits = 0;
    }
}
static uint32_t tdfl_rev(uint32_t code, int len) {
    uint32_t r = 0;
    for (int i = 0; i < len; i++) { r = (r << 1) | (code & 1); code >>= 1; }
    return r;
}

/* Emit a literal/length symbol using the fixed Huffman code (MSB-first). */
static void tdfl_sym(tdfl_bw *w, int sym) {
    uint32_t code; int len;
    if (sym <= 143)      { code = 0x30 + sym;         len = 8; }
    else if (sym <= 255) { code = 0x190 + (sym-144);  len = 9; }
    else if (sym <= 279) { code = sym - 256;          len = 7; }
    else                 { code = 0xC0 + (sym-280);   len = 8; }
    tdfl_bits(w, tdfl_rev(code, len), len);
}

static void tdfl_match(tdfl_bw *w, int len, int dist) {
    int i = 0; while (i < 28 && len >= tdfl_lenb[i+1]) i++;
    tdfl_sym(w, 257 + i);
    if (tdfl_lene[i]) tdfl_bits(w, (uint32_t)(len - tdfl_lenb[i]), tdfl_lene[i]);
    int d = 0; while (d < 29 && dist >= tdfl_distb[d+1]) d++;
    tdfl_bits(w, tdfl_rev((uint32_t)d, 5), 5);                 /* 5-bit fixed distance code */
    if (tdfl_diste[d]) tdfl_bits(w, (uint32_t)(dist - tdfl_distb[d]), tdfl_diste[d]);
}

#define TDFL_WSIZE   32768
#define TDFL_MINM    3
#define TDFL_MAXM    258
#define TDFL_HBITS   15
#define TDFL_HSIZE   (1 << TDFL_HBITS)

static uint32_t tdfl_hash(const uint8_t *p) {
    uint32_t h = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
    return ((h * 2654435761u) >> (32 - TDFL_HBITS)) & (TDFL_HSIZE - 1);
}

/* Compress src into a raw DEFLATE stream (single fixed-Huffman block). */
static long tdfl_deflate(const uint8_t *src, size_t slen, uint8_t *dst, size_t dcap) {
    tdfl_bw w = { dst, dcap, 0, 0, 0, 0 };
    tdfl_bits(&w, 1, 1);                                       /* BFINAL = 1 */
    tdfl_bits(&w, 1, 2);                                       /* BTYPE  = 01 (fixed) */
    if (slen) {
        int32_t *head = (int32_t *)malloc(sizeof(int32_t) * TDFL_HSIZE);
        int32_t *prev = (int32_t *)malloc(sizeof(int32_t) * slen);
        if (!head || !prev) { free(head); free(prev); return -1; }
        for (int i = 0; i < TDFL_HSIZE; i++) head[i] = -1;
        size_t i = 0;
        while (i < slen) {
            int bestlen = 0, bestdist = 0;
            if (i + TDFL_MINM <= slen) {
                uint32_t h = tdfl_hash(src + i);
                int32_t j = head[h];
                int attempts = 128;
                size_t maxl = slen - i; if (maxl > TDFL_MAXM) maxl = TDFL_MAXM;
                while (j >= 0 && attempts--) {
                    size_t dist = i - (size_t)j;
                    if (dist > TDFL_WSIZE) break;
                    size_t l = 0;
                    while (l < maxl && src[(size_t)j + l] == src[i + l]) l++;
                    if ((int)l > bestlen) { bestlen = (int)l; bestdist = (int)dist; if (l >= maxl) break; }
                    j = prev[j];
                }
            }
            if (bestlen >= TDFL_MINM) {
                tdfl_match(&w, bestlen, bestdist);
                size_t end = i + bestlen;
                while (i < end) {
                    if (i + TDFL_MINM <= slen) { uint32_t h = tdfl_hash(src + i); prev[i] = head[h]; head[h] = (int32_t)i; }
                    i++;
                }
            } else {
                tdfl_sym(&w, src[i]);
                if (i + TDFL_MINM <= slen) { uint32_t h = tdfl_hash(src + i); prev[i] = head[h]; head[h] = (int32_t)i; }
                i++;
            }
            if (w.err) { free(head); free(prev); return -1; }
        }
        free(head); free(prev);
    }
    tdfl_sym(&w, 256);                                         /* end of block */
    tdfl_flush(&w);
    return w.err ? -1 : (long)w.len;
}

static uint32_t tdfl_adler32(const uint8_t *d, size_t n) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; i++) { a += d[i]; if (a >= 65521) a -= 65521; b += a; if (b >= 65521) b -= 65521; }
    return (b << 16) | a;
}

/* Compress src into a zlib stream (2-byte header + DEFLATE + adler32).
 * Returns the stream length, or -1 if it would not fit in dcap. */
static long tdfl_zlib(const uint8_t *src, size_t slen, uint8_t *dst, size_t dcap) {
    if (dcap < 6) return -1;
    dst[0] = 0x78; dst[1] = 0x9c;                             /* CM=deflate, 32K window, FCHECK ok */
    long n = tdfl_deflate(src, slen, dst + 2, dcap - 6);
    if (n < 0) return -1;
    size_t p = 2 + (size_t)n;
    uint32_t a = tdfl_adler32(src, slen);
    dst[p++] = (uint8_t)(a >> 24); dst[p++] = (uint8_t)(a >> 16);
    dst[p++] = (uint8_t)(a >> 8);  dst[p++] = (uint8_t)a;
    return (long)p;
}

#endif /* TDEFLATE_H */
