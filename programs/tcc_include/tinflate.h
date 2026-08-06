/* SPDX-License-Identifier: GPL-2.0-only */

/* TinyOS userspace DEFLATE/zlib decompressor — Cycle 9 C6a (git over HTTPS).
 *
 * git packfiles store every object zlib-compressed (RFC 1950 wrapper around a
 * raw DEFLATE stream, RFC 1951), so cloning a repository needs an inflate.
 * This is a self-contained, allocation-free inflate: the caller supplies the
 * output buffer, and every entry point reports how many *input* bytes were
 * consumed so the packfile reader can find where the next object begins.
 *
 * The Huffman decoder follows Mark Adler's public-domain puff.c construction
 * (canonical codes via per-length counts + a sorted symbol table).  Plain C so
 * on-device TinyCC accepts it; only memcpy/memset are pulled from <string.h>.
 *
 * Verified on the host against zlib (stored, fixed and dynamic blocks, multi-
 * block streams, adler32) — see the C6a self-test. */
#ifndef TINFLATE_H
#define TINFLATE_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* Decoder working state (bit reader + bounded output cursor). */
typedef struct {
    const uint8_t *in;
    size_t inlen, inpos;
    uint32_t bitbuf;        /* unconsumed bits of the current input byte */
    int bitcnt;             /* number of valid bits in bitbuf */
    uint8_t *out;
    size_t outcap, outpos;
    int err;                /* set when the input underruns */
} tinfl_s;

/* Canonical Huffman code table: how many codes exist of each length (1..15)
 * plus the symbols in canonical (length, then value) order. */
typedef struct {
    short counts[16];
    short symbols[288];
} tinfl_huff;

/* ------------------------------------------------------------------ */
/* bit reader (DEFLATE packs bits LSB-first within each byte)          */
/* ------------------------------------------------------------------ */
static int tinfl_getbit(tinfl_s *s) {
    if (s->bitcnt == 0) {
        if (s->inpos >= s->inlen) { s->err = 1; return 0; }
        s->bitbuf = s->in[s->inpos++];
        s->bitcnt = 8;
    }
    int b = s->bitbuf & 1;
    s->bitbuf >>= 1;
    s->bitcnt--;
    return b;
}

static int tinfl_bits(tinfl_s *s, int need) {
    int val = 0, i;
    for (i = 0; i < need; i++) val |= tinfl_getbit(s) << i;
    return val;
}

/* ------------------------------------------------------------------ */
/* Huffman tables                                                     */
/* ------------------------------------------------------------------ */
/* Build a canonical table from a list of code lengths.  Returns 0 when the
 * code is complete, >0 when incomplete (under-subscribed), <0 when it is
 * over-subscribed (corrupt). */
static int tinfl_construct(tinfl_huff *h, const short *lengths, int n) {
    int symbol, len, left;
    short offs[16];
    for (len = 0; len <= 15; len++) h->counts[len] = 0;
    for (symbol = 0; symbol < n; symbol++) h->counts[lengths[symbol]]++;
    if (h->counts[0] == n) return 0;            /* no codes — vacuously complete */

    left = 1;
    for (len = 1; len <= 15; len++) {
        left <<= 1;
        left -= h->counts[len];
        if (left < 0) return left;              /* over-subscribed */
    }

    offs[1] = 0;
    for (len = 1; len < 15; len++) offs[len + 1] = offs[len] + h->counts[len];
    for (symbol = 0; symbol < n; symbol++)
        if (lengths[symbol] != 0) h->symbols[offs[lengths[symbol]]++] = symbol;
    return left;                                /* 0 = complete, >0 = incomplete */
}

/* Decode one symbol by walking code lengths, accumulating the code MSB-first. */
static int tinfl_decode(tinfl_s *s, const tinfl_huff *h) {
    int code = 0, first = 0, index = 0, len, count;
    for (len = 1; len <= 15; len++) {
        code |= tinfl_getbit(s);
        count = h->counts[len];
        if (code - count < first) return h->symbols[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;                                  /* ran out of codes */
}

/* ------------------------------------------------------------------ */
/* DEFLATE (RFC 1951)                                                 */
/* ------------------------------------------------------------------ */
static const short tinfl_len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
    67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const short tinfl_len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
    4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const short tinfl_dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513,
    769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const short tinfl_dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
    9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

/* Inflate a compressed block given its literal/length and distance tables. */
static int tinfl_codes(tinfl_s *s, const tinfl_huff *lh, const tinfl_huff *dh) {
    for (;;) {
        int sym = tinfl_decode(s, lh);
        if (s->err || sym < 0) return -1;
        if (sym == 256) return 0;               /* end of block */
        if (sym < 256) {
            if (s->outpos >= s->outcap) return -1;
            s->out[s->outpos++] = (uint8_t)sym;
        } else {
            sym -= 257;
            if (sym >= 29) return -1;
            int len = tinfl_len_base[sym] + tinfl_bits(s, tinfl_len_extra[sym]);
            sym = tinfl_decode(s, dh);
            if (sym < 0 || sym >= 30) return -1;
            int dist = tinfl_dist_base[sym] + tinfl_bits(s, tinfl_dist_extra[sym]);
            if ((size_t)dist > s->outpos) return -1;         /* bad back-reference */
            if (s->outpos + (size_t)len > s->outcap) return -1;
            size_t from = s->outpos - dist;
            while (len--) { s->out[s->outpos] = s->out[from++]; s->outpos++; }
        }
        if (s->err) return -1;
    }
}

/* Copy an uncompressed (stored) block. */
static int tinfl_stored(tinfl_s *s) {
    s->bitbuf = 0; s->bitcnt = 0;               /* align to the next byte */
    if (s->inpos + 4 > s->inlen) return -1;
    unsigned len  = s->in[s->inpos] | (s->in[s->inpos + 1] << 8);
    unsigned nlen = s->in[s->inpos + 2] | (s->in[s->inpos + 3] << 8);
    s->inpos += 4;
    if ((len ^ 0xFFFFu) != nlen) return -1;
    if (s->inpos + len > s->inlen) return -1;
    if (s->outpos + len > s->outcap) return -1;
    memcpy(s->out + s->outpos, s->in + s->inpos, len);
    s->inpos += len; s->outpos += len;
    return 0;
}

/* Build the RFC 1951 fixed Huffman tables. */
static void tinfl_fixed(tinfl_huff *lh, tinfl_huff *dh) {
    short lengths[288];
    int i;
    for (i = 0;   i < 144; i++) lengths[i] = 8;
    for (;        i < 256; i++) lengths[i] = 9;
    for (;        i < 280; i++) lengths[i] = 7;
    for (;        i < 288; i++) lengths[i] = 8;
    tinfl_construct(lh, lengths, 288);
    for (i = 0; i < 30; i++) lengths[i] = 5;
    tinfl_construct(dh, lengths, 30);
}

/* Read the dynamic Huffman code descriptions and build both tables. */
static int tinfl_dynamic(tinfl_s *s, tinfl_huff *lh, tinfl_huff *dh) {
    static const short order[19] =
        { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    short lengths[288 + 32];
    tinfl_huff codelen;
    int nlen  = tinfl_bits(s, 5) + 257;
    int ndist = tinfl_bits(s, 5) + 1;
    int ncode = tinfl_bits(s, 4) + 4;
    int index, err;
    if (nlen > 286 || ndist > 30) return -1;

    for (index = 0; index < ncode; index++) lengths[order[index]] = tinfl_bits(s, 3);
    for (; index < 19; index++) lengths[order[index]] = 0;
    if (tinfl_construct(&codelen, lengths, 19) != 0) return -1;

    index = 0;
    while (index < nlen + ndist) {
        int sym = tinfl_decode(s, &codelen);
        if (s->err || sym < 0) return -1;
        if (sym < 16) {
            lengths[index++] = sym;
        } else {
            int rep = 0;
            short val = 0;
            if (sym == 16) {
                if (index == 0) return -1;
                val = lengths[index - 1];
                rep = 3 + tinfl_bits(s, 2);
            } else if (sym == 17) {
                rep = 3 + tinfl_bits(s, 3);
            } else {
                rep = 11 + tinfl_bits(s, 7);
            }
            if (index + rep > nlen + ndist) return -1;
            while (rep--) lengths[index++] = val;
        }
    }
    if (lengths[256] == 0) return -1;           /* missing end-of-block code */

    err = tinfl_construct(lh, lengths, nlen);
    if (err && (err < 0 || nlen != lh->counts[0] + lh->counts[1])) return -1;
    err = tinfl_construct(dh, lengths + nlen, ndist);
    if (err && (err < 0 || ndist != dh->counts[0] + dh->counts[1])) return -1;
    return 0;
}

/* Inflate a whole raw DEFLATE stream into s->out. */
static int tinfl_inflate(tinfl_s *s) {
    int last;
    tinfl_huff lh, dh;
    do {
        last = tinfl_getbit(s);
        int type = tinfl_bits(s, 2);
        if (s->err) return -1;
        if (type == 0) {
            if (tinfl_stored(s)) return -1;
        } else if (type == 1) {
            tinfl_fixed(&lh, &dh);
            if (tinfl_codes(s, &lh, &dh)) return -1;
        } else if (type == 2) {
            if (tinfl_dynamic(s, &lh, &dh)) return -1;
            if (tinfl_codes(s, &lh, &dh)) return -1;
        } else {
            return -1;                          /* reserved block type */
        }
    } while (!last);
    return 0;
}

/* ------------------------------------------------------------------ */
/* public API                                                         */
/* ------------------------------------------------------------------ */
/* Inflate raw DEFLATE.  Returns the output length or -1; *consumed (optional)
 * receives the number of input bytes read (rounded up to the final byte). */
static long tinfl_raw(const uint8_t *src, size_t srclen,
                      uint8_t *dst, size_t dstcap, size_t *consumed) {
    tinfl_s s;
    memset(&s, 0, sizeof s);
    s.in = src; s.inlen = srclen; s.out = dst; s.outcap = dstcap;
    if (tinfl_inflate(&s)) return -1;
    if (consumed) *consumed = s.inpos;
    return (long)s.outpos;
}

static uint32_t tinfl_adler32(const uint8_t *d, size_t n) {
    uint32_t a = 1, b = 0;
    size_t i;
    for (i = 0; i < n; i++) {
        a += d[i]; if (a >= 65521) a -= 65521;
        b += a;    if (b >= 65521) b -= 65521;
    }
    return (b << 16) | a;
}

/* Inflate a zlib stream (RFC 1950): 2-byte header, DEFLATE body, adler32
 * trailer.  The header check bits and (when present) the adler32 are verified.
 * *consumed receives header + body + 4 trailer bytes. */
static long tinfl_zlib(const uint8_t *src, size_t srclen,
                       uint8_t *dst, size_t dstcap, size_t *consumed) {
    if (srclen < 2) return -1;
    int cmf = src[0], flg = src[1];
    if ((cmf & 0x0f) != 8) return -1;           /* method must be DEFLATE */
    if (((cmf << 8) | flg) % 31 != 0) return -1; /* FCHECK */
    if (flg & 0x20) return -1;                  /* preset dictionaries unsupported */

    size_t used = 0;
    long out = tinfl_raw(src + 2, srclen - 2, dst, dstcap, &used);
    if (out < 0) return -1;

    size_t end = 2 + used;
    if (end + 4 <= srclen) {                     /* verify the adler32 trailer */
        uint32_t want = ((uint32_t)src[end] << 24) | ((uint32_t)src[end + 1] << 16) |
                        ((uint32_t)src[end + 2] << 8) | (uint32_t)src[end + 3];
        if (tinfl_adler32(dst, (size_t)out) != want) return -1;
        end += 4;
    }
    if (consumed) *consumed = end;
    return out;
}

#endif /* TINFLATE_H */
