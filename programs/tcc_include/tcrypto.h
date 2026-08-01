/* TinyOS userspace crypto library — Cycle 9 (secure comms: HTTPS / git / SSH).
 *
 * Phase C1: the hashing primitives that everything else is built on —
 * SHA-256, SHA-1, and HMAC-SHA256.  Portable, dependency-free (only memcpy/
 * memset from <string.h>), plain C so on-device TinyCC accepts it, and used by
 * the TLS record/handshake layers added in later phases.
 *
 * All implementations are the textbook FIPS 180-4 / RFC 2104 algorithms and are
 * checked against published test vectors (see the C9 self-test). */
#ifndef TCRYPTO_H
#define TCRYPTO_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define TC_ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

/* ======================================================================== */
/* SHA-256 (FIPS 180-4)                                                      */
/* ======================================================================== */
typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; int n; } sha256_ctx;

static const uint32_t tc_sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static void sha256_init(sha256_ctx *c) {
    c->h[0] = 0x6a09e667; c->h[1] = 0xbb67ae85; c->h[2] = 0x3c6ef372; c->h[3] = 0xa54ff53a;
    c->h[4] = 0x510e527f; c->h[5] = 0x9b05688c; c->h[6] = 0x1f83d9ab; c->h[7] = 0x5be0cd19;
    c->len = 0; c->n = 0;
}
static void sha256_block(sha256_ctx *c, const uint8_t *p) {
    uint32_t w[64], a, b, cc, d, e, f, g, h, t1, t2;
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = TC_ROR32(w[i - 15], 7) ^ TC_ROR32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = TC_ROR32(w[i - 2], 17) ^ TC_ROR32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = TC_ROR32(e, 6) ^ TC_ROR32(e, 11) ^ TC_ROR32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        t1 = h + S1 + ch + tc_sha256_k[i] + w[i];
        uint32_t S0 = TC_ROR32(a, 2) ^ TC_ROR32(a, 13) ^ TC_ROR32(a, 22);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}
static void sha256_update(sha256_ctx *c, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    c->len += len;
    while (len) {
        size_t k = 64 - (size_t)c->n; if (k > len) k = len;
        memcpy(c->buf + c->n, p, k); c->n += (int)k; p += k; len -= k;
        if (c->n == 64) { sha256_block(c, c->buf); c->n = 0; }
    }
}
static void sha256_final(sha256_ctx *c, uint8_t out[32]) {
    uint64_t bits = c->len * 8;                    /* message length captured before padding */
    uint8_t pad = 0x80, z = 0;
    sha256_update(c, &pad, 1);
    while (c->n != 56) sha256_update(c, &z, 1);
    uint8_t lb[8]; for (int i = 0; i < 8; i++) lb[i] = (uint8_t)(bits >> (56 - i * 8));
    sha256_update(c, lb, 8);
    for (int i = 0; i < 8; i++) {
        out[i * 4] = (uint8_t)(c->h[i] >> 24); out[i * 4 + 1] = (uint8_t)(c->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c->h[i] >> 8); out[i * 4 + 3] = (uint8_t)c->h[i];
    }
}
static void sha256(const void *data, size_t len, uint8_t out[32]) {
    sha256_ctx c; sha256_init(&c); sha256_update(&c, data, len); sha256_final(&c, out);
}

/* ======================================================================== */
/* SHA-1 (FIPS 180-4) — needed for git object IDs and legacy TLS            */
/* ======================================================================== */
typedef struct { uint32_t h[5]; uint64_t len; uint8_t buf[64]; int n; } sha1_ctx;

static void sha1_init(sha1_ctx *c) {
    c->h[0] = 0x67452301; c->h[1] = 0xEFCDAB89; c->h[2] = 0x98BADCFE; c->h[3] = 0x10325476; c->h[4] = 0xC3D2E1F0;
    c->len = 0; c->n = 0;
}
static void sha1_block(sha1_ctx *c, const uint8_t *p) {
    uint32_t w[80], a, b, cc, d, e, t;
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 80; i++) { uint32_t x = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16]; w[i] = (x << 1) | (x >> 31); }
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & cc) | (~b & d);            k = 0x5A827999; }
        else if (i < 40) { f = b ^ cc ^ d;                    k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & cc) | (b & d) | (cc & d); k = 0x8F1BBCDC; }
        else             { f = b ^ cc ^ d;                    k = 0xCA62C1D6; }
        t = ((a << 5) | (a >> 27)) + f + e + k + w[i];
        e = d; d = cc; cc = (b << 30) | (b >> 2); b = a; a = t;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
}
static void sha1_update(sha1_ctx *c, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    c->len += len;
    while (len) {
        size_t k = 64 - (size_t)c->n; if (k > len) k = len;
        memcpy(c->buf + c->n, p, k); c->n += (int)k; p += k; len -= k;
        if (c->n == 64) { sha1_block(c, c->buf); c->n = 0; }
    }
}
static void sha1_final(sha1_ctx *c, uint8_t out[20]) {
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80, z = 0;
    sha1_update(c, &pad, 1);
    while (c->n != 56) sha1_update(c, &z, 1);
    uint8_t lb[8]; for (int i = 0; i < 8; i++) lb[i] = (uint8_t)(bits >> (56 - i * 8));
    sha1_update(c, lb, 8);
    for (int i = 0; i < 5; i++) {
        out[i * 4] = (uint8_t)(c->h[i] >> 24); out[i * 4 + 1] = (uint8_t)(c->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c->h[i] >> 8); out[i * 4 + 3] = (uint8_t)c->h[i];
    }
}
static void sha1(const void *data, size_t len, uint8_t out[20]) {
    sha1_ctx c; sha1_init(&c); sha1_update(&c, data, len); sha1_final(&c, out);
}

/* ======================================================================== */
/* SHA-512 (FIPS 180-4) — needed for Ed25519 (SSH host keys / client auth)   */
/* ======================================================================== */
#define TC_ROR64(x, n) (((x) >> (n)) | ((x) << (64 - (n))))
typedef struct { uint64_t h[8]; uint64_t len; uint8_t buf[128]; int n; } sha512_ctx;

static const uint64_t tc_sha512_k[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL
};

static void sha512_init(sha512_ctx *c) {
    c->h[0] = 0x6a09e667f3bcc908ULL; c->h[1] = 0xbb67ae8584caa73bULL;
    c->h[2] = 0x3c6ef372fe94f82bULL; c->h[3] = 0xa54ff53a5f1d36f1ULL;
    c->h[4] = 0x510e527fade682d1ULL; c->h[5] = 0x9b05688c2b3e6c1fULL;
    c->h[6] = 0x1f83d9abfb41bd6bULL; c->h[7] = 0x5be0cd19137e2179ULL;
    c->len = 0; c->n = 0;
}
static void sha512_block(sha512_ctx *c, const uint8_t *p) {
    uint64_t w[80], a, b, cc, d, e, f, g, h, t1, t2;
    for (int i = 0; i < 16; i++) {
        w[i] = 0;
        for (int j = 0; j < 8; j++) w[i] = (w[i] << 8) | p[i * 8 + j];
    }
    for (int i = 16; i < 80; i++) {
        uint64_t s0 = TC_ROR64(w[i-15], 1) ^ TC_ROR64(w[i-15], 8) ^ (w[i-15] >> 7);
        uint64_t s1 = TC_ROR64(w[i-2], 19) ^ TC_ROR64(w[i-2], 61) ^ (w[i-2] >> 6);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3];
    e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
    for (int i = 0; i < 80; i++) {
        uint64_t S1 = TC_ROR64(e, 14) ^ TC_ROR64(e, 18) ^ TC_ROR64(e, 41);
        uint64_t ch = (e & f) ^ (~e & g);
        t1 = h + S1 + ch + tc_sha512_k[i] + w[i];
        uint64_t S0 = TC_ROR64(a, 28) ^ TC_ROR64(a, 34) ^ TC_ROR64(a, 39);
        uint64_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}
static void sha512_update(sha512_ctx *c, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    c->len += len;
    while (len) {
        size_t k = 128 - (size_t)c->n; if (k > len) k = len;
        memcpy(c->buf + c->n, p, k); c->n += (int)k; p += k; len -= k;
        if (c->n == 128) { sha512_block(c, c->buf); c->n = 0; }
    }
}
static void sha512_final(sha512_ctx *c, uint8_t out[64]) {
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80, z = 0;
    sha512_update(c, &pad, 1);
    while (c->n != 112) sha512_update(c, &z, 1);
    uint8_t lb[16]; memset(lb, 0, 16);
    for (int i = 0; i < 8; i++) lb[8 + i] = (uint8_t)(bits >> (56 - i * 8));
    sha512_update(c, lb, 16);
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++) out[i * 8 + j] = (uint8_t)(c->h[i] >> (56 - j * 8));
}
static void sha512(const void *data, size_t len, uint8_t out[64]) {
    sha512_ctx c; sha512_init(&c); sha512_update(&c, data, len); sha512_final(&c, out);
}

/* ======================================================================== */
/* HMAC-SHA256 (RFC 2104) — streaming + one-shot; the basis for HKDF          */
/* ======================================================================== */
typedef struct { sha256_ctx inner; uint8_t opad[64]; } hmac_sha256_ctx;

static void hmac_sha256_init(hmac_sha256_ctx *c, const void *key, size_t klen) {
    uint8_t k[64], ipad[64];
    memset(k, 0, 64);
    if (klen > 64) sha256(key, klen, k); else memcpy(k, key, klen);
    for (int i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; c->opad[i] = k[i] ^ 0x5c; }
    sha256_init(&c->inner); sha256_update(&c->inner, ipad, 64);
}
static void hmac_sha256_update(hmac_sha256_ctx *c, const void *data, size_t len) { sha256_update(&c->inner, data, len); }
static void hmac_sha256_final(hmac_sha256_ctx *c, uint8_t out[32]) {
    uint8_t ih[32]; sha256_final(&c->inner, ih);
    sha256_ctx o; sha256_init(&o); sha256_update(&o, c->opad, 64); sha256_update(&o, ih, 32); sha256_final(&o, out);
}
static void hmac_sha256(const void *key, size_t klen, const void *data, size_t dlen, uint8_t out[32]) {
    hmac_sha256_ctx c; hmac_sha256_init(&c, key, klen); hmac_sha256_update(&c, data, dlen); hmac_sha256_final(&c, out);
}

/* ======================================================================== */
/* HKDF-SHA256 (RFC 5869) — TLS 1.3 key derivation                           */
/* ======================================================================== */
static void hkdf_extract(const uint8_t *salt, size_t saltlen, const uint8_t *ikm, size_t ikmlen, uint8_t prk[32]) {
    uint8_t zero[32];
    if (!salt || saltlen == 0) { memset(zero, 0, 32); salt = zero; saltlen = 32; }
    hmac_sha256(salt, saltlen, ikm, ikmlen, prk);
}
static void hkdf_expand(const uint8_t prk[32], const uint8_t *info, size_t infolen, uint8_t *okm, size_t okmlen) {
    uint8_t t[32]; size_t tlen = 0, done = 0; uint8_t ctr = 1;
    while (done < okmlen) {
        hmac_sha256_ctx c; hmac_sha256_init(&c, prk, 32);
        if (tlen) hmac_sha256_update(&c, t, tlen);
        hmac_sha256_update(&c, info, infolen);
        hmac_sha256_update(&c, &ctr, 1);
        hmac_sha256_final(&c, t); tlen = 32;
        size_t n = okmlen - done; if (n > 32) n = 32;
        memcpy(okm + done, t, n); done += n; ctr++;
    }
}

/* ---- little-endian helpers (ChaCha20 + Poly1305) ---- */
#define TC_ROL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
static uint32_t tc_u8to32le(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void tc_u32to8le(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* ======================================================================== */
/* ChaCha20 (RFC 8439)                                                       */
/* ======================================================================== */
#define TC_QR(a, b, c, d) do { \
    a += b; d ^= a; d = TC_ROL32(d, 16); \
    c += d; b ^= c; b = TC_ROL32(b, 12); \
    a += b; d ^= a; d = TC_ROL32(d, 8);  \
    c += d; b ^= c; b = TC_ROL32(b, 7);  \
} while (0)

static void chacha20_setup(uint32_t st[16], const uint8_t key[32], uint32_t counter, const uint8_t nonce[12]) {
    st[0] = 0x61707865; st[1] = 0x3320646e; st[2] = 0x79622d32; st[3] = 0x6b206574;
    for (int i = 0; i < 8; i++) st[4 + i] = tc_u8to32le(key + i * 4);
    st[12] = counter;
    st[13] = tc_u8to32le(nonce + 0); st[14] = tc_u8to32le(nonce + 4); st[15] = tc_u8to32le(nonce + 8);
}
static void chacha20_block(const uint32_t in[16], uint8_t out[64]) {
    uint32_t x[16];
    for (int i = 0; i < 16; i++) x[i] = in[i];
    for (int i = 0; i < 10; i++) {
        TC_QR(x[0], x[4], x[8],  x[12]); TC_QR(x[1], x[5], x[9],  x[13]);
        TC_QR(x[2], x[6], x[10], x[14]); TC_QR(x[3], x[7], x[11], x[15]);
        TC_QR(x[0], x[5], x[10], x[15]); TC_QR(x[1], x[6], x[11], x[12]);
        TC_QR(x[2], x[7], x[8],  x[13]); TC_QR(x[3], x[4], x[9],  x[14]);
    }
    for (int i = 0; i < 16; i++) tc_u32to8le(out + i * 4, x[i] + in[i]);
}
/* Encrypt or decrypt (XOR keystream); same operation both ways. */
static void chacha20_xor(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12],
                         const uint8_t *in, uint8_t *out, size_t len) {
    uint32_t st[16]; chacha20_setup(st, key, counter, nonce);
    uint8_t ks[64];
    while (len) {
        chacha20_block(st, ks); st[12]++;
        size_t n = len < 64 ? len : 64;
        for (size_t i = 0; i < n; i++) out[i] = in[i] ^ ks[i];
        in += n; out += n; len -= n;
    }
}

/* ======================================================================== */
/* Poly1305 (RFC 8439) — poly1305-donna 32-bit (public domain)               */
/* ======================================================================== */
typedef struct { uint32_t r[5], h[5], pad[4]; size_t leftover; uint8_t buffer[16]; uint8_t final; } poly1305_ctx;

static void poly1305_init(poly1305_ctx *st, const uint8_t key[32]) {
    st->r[0] = (tc_u8to32le(&key[0])     ) & 0x3ffffff;
    st->r[1] = (tc_u8to32le(&key[3]) >> 2) & 0x3ffff03;
    st->r[2] = (tc_u8to32le(&key[6]) >> 4) & 0x3ffc0ff;
    st->r[3] = (tc_u8to32le(&key[9]) >> 6) & 0x3f03fff;
    st->r[4] = (tc_u8to32le(&key[12]) >> 8) & 0x00fffff;
    st->h[0] = st->h[1] = st->h[2] = st->h[3] = st->h[4] = 0;
    st->pad[0] = tc_u8to32le(&key[16]); st->pad[1] = tc_u8to32le(&key[20]);
    st->pad[2] = tc_u8to32le(&key[24]); st->pad[3] = tc_u8to32le(&key[28]);
    st->leftover = 0; st->final = 0;
}
static void poly1305_blocks(poly1305_ctx *st, const uint8_t *m, size_t bytes) {
    const uint32_t hibit = st->final ? 0 : (1u << 24);
    uint32_t r0 = st->r[0], r1 = st->r[1], r2 = st->r[2], r3 = st->r[3], r4 = st->r[4];
    uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
    uint32_t h0 = st->h[0], h1 = st->h[1], h2 = st->h[2], h3 = st->h[3], h4 = st->h[4];
    while (bytes >= 16) {
        uint64_t d0, d1, d2, d3, d4; uint32_t c;
        h0 += (tc_u8to32le(m + 0)     ) & 0x3ffffff;
        h1 += (tc_u8to32le(m + 3) >> 2) & 0x3ffffff;
        h2 += (tc_u8to32le(m + 6) >> 4) & 0x3ffffff;
        h3 += (tc_u8to32le(m + 9) >> 6) & 0x3ffffff;
        h4 += (tc_u8to32le(m + 12) >> 8) | hibit;
        d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 + (uint64_t)h2 * s3 + (uint64_t)h3 * s2 + (uint64_t)h4 * s1;
        d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 + (uint64_t)h2 * s4 + (uint64_t)h3 * s3 + (uint64_t)h4 * s2;
        d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 + (uint64_t)h2 * r0 + (uint64_t)h3 * s4 + (uint64_t)h4 * s3;
        d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 + (uint64_t)h2 * r1 + (uint64_t)h3 * r0 + (uint64_t)h4 * s4;
        d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 + (uint64_t)h2 * r2 + (uint64_t)h3 * r1 + (uint64_t)h4 * r0;
        c = (uint32_t)(d0 >> 26); h0 = (uint32_t)d0 & 0x3ffffff;
        d1 += c; c = (uint32_t)(d1 >> 26); h1 = (uint32_t)d1 & 0x3ffffff;
        d2 += c; c = (uint32_t)(d2 >> 26); h2 = (uint32_t)d2 & 0x3ffffff;
        d3 += c; c = (uint32_t)(d3 >> 26); h3 = (uint32_t)d3 & 0x3ffffff;
        d4 += c; c = (uint32_t)(d4 >> 26); h4 = (uint32_t)d4 & 0x3ffffff;
        h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;
        m += 16; bytes -= 16;
    }
    st->h[0] = h0; st->h[1] = h1; st->h[2] = h2; st->h[3] = h3; st->h[4] = h4;
}
static void poly1305_update(poly1305_ctx *st, const uint8_t *m, size_t bytes) {
    if (st->leftover) {
        size_t want = 16 - st->leftover; if (want > bytes) want = bytes;
        for (size_t i = 0; i < want; i++) st->buffer[st->leftover + i] = m[i];
        bytes -= want; m += want; st->leftover += want;
        if (st->leftover < 16) return;
        poly1305_blocks(st, st->buffer, 16); st->leftover = 0;
    }
    if (bytes >= 16) { size_t want = bytes & ~(size_t)15; poly1305_blocks(st, m, want); m += want; bytes -= want; }
    for (size_t i = 0; i < bytes; i++) st->buffer[st->leftover + i] = m[i];
    st->leftover += bytes;
}
static void poly1305_finish(poly1305_ctx *st, uint8_t mac[16]) {
    if (st->leftover) {
        size_t i = st->leftover; st->buffer[i++] = 1;
        for (; i < 16; i++) st->buffer[i] = 0;
        st->final = 1; poly1305_blocks(st, st->buffer, 16);
    }
    uint32_t h0 = st->h[0], h1 = st->h[1], h2 = st->h[2], h3 = st->h[3], h4 = st->h[4], c;
    c = h1 >> 26; h1 &= 0x3ffffff;
    h2 += c; c = h2 >> 26; h2 &= 0x3ffffff;
    h3 += c; c = h3 >> 26; h3 &= 0x3ffffff;
    h4 += c; c = h4 >> 26; h4 &= 0x3ffffff;
    h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;
    uint32_t g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffff;
    uint32_t g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffff;
    uint32_t g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffff;
    uint32_t g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffff;
    uint32_t g4 = h4 + c - (1u << 26);
    uint32_t mask = (g4 >> 31) - 1;
    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0; h1 = (h1 & mask) | g1; h2 = (h2 & mask) | g2; h3 = (h3 & mask) | g3; h4 = (h4 & mask) | g4;
    h0 = (h0      ) | (h1 << 26);
    h1 = (h1 >>  6) | (h2 << 20);
    h2 = (h2 >> 12) | (h3 << 14);
    h3 = (h3 >> 18) | (h4 <<  8);
    uint64_t f;
    f = (uint64_t)h0 + st->pad[0];             h0 = (uint32_t)f;
    f = (uint64_t)h1 + st->pad[1] + (f >> 32); h1 = (uint32_t)f;
    f = (uint64_t)h2 + st->pad[2] + (f >> 32); h2 = (uint32_t)f;
    f = (uint64_t)h3 + st->pad[3] + (f >> 32); h3 = (uint32_t)f;
    tc_u32to8le(mac + 0, h0); tc_u32to8le(mac + 4, h1); tc_u32to8le(mac + 8, h2); tc_u32to8le(mac + 12, h3);
}
static void poly1305(const uint8_t key[32], const uint8_t *m, size_t len, uint8_t mac[16]) {
    poly1305_ctx st; poly1305_init(&st, key); poly1305_update(&st, m, len); poly1305_finish(&st, mac);
}

/* ======================================================================== */
/* ChaCha20-Poly1305 AEAD (RFC 8439 §2.8) — the TLS 1.3 record cipher        */
/* ======================================================================== */
static void tc_aead_mac(const uint8_t polykey[32], const uint8_t *aad, size_t aadlen,
                        const uint8_t *ct, size_t ctlen, uint8_t tag[16]) {
    static const uint8_t zeros[16] = { 0 };
    poly1305_ctx st; poly1305_init(&st, polykey);
    poly1305_update(&st, aad, aadlen);
    if (aadlen % 16) poly1305_update(&st, zeros, 16 - (aadlen % 16));
    poly1305_update(&st, ct, ctlen);
    if (ctlen % 16) poly1305_update(&st, zeros, 16 - (ctlen % 16));
    uint8_t lens[16];
    for (int i = 0; i < 8; i++) lens[i]     = (uint8_t)((uint64_t)aadlen >> (8 * i));
    for (int i = 0; i < 8; i++) lens[8 + i] = (uint8_t)((uint64_t)ctlen  >> (8 * i));
    poly1305_update(&st, lens, 16);
    poly1305_finish(&st, tag);
}
static void chacha20poly1305_encrypt(const uint8_t key[32], const uint8_t nonce[12],
        const uint8_t *aad, size_t aadlen, const uint8_t *pt, size_t ptlen,
        uint8_t *ct, uint8_t tag[16]) {
    uint32_t st[16]; uint8_t polykey[64];
    chacha20_setup(st, key, 0, nonce); chacha20_block(st, polykey);   /* counter 0 -> Poly1305 key */
    chacha20_xor(key, 1, nonce, pt, ct, ptlen);                       /* counter 1.. -> data */
    tc_aead_mac(polykey, aad, aadlen, ct, ptlen, tag);
}
/* Returns 0 if the tag verifies (plaintext written), -1 on authentication failure. */
static int chacha20poly1305_decrypt(const uint8_t key[32], const uint8_t nonce[12],
        const uint8_t *aad, size_t aadlen, const uint8_t *ct, size_t ctlen, const uint8_t tag[16],
        uint8_t *pt) {
    uint32_t st[16]; uint8_t polykey[64], calc[16];
    chacha20_setup(st, key, 0, nonce); chacha20_block(st, polykey);
    tc_aead_mac(polykey, aad, aadlen, ct, ctlen, calc);
    uint8_t diff = 0; for (int i = 0; i < 16; i++) diff |= calc[i] ^ tag[i];   /* constant-time */
    if (diff) return -1;
    chacha20_xor(key, 1, nonce, ct, pt, ctlen);
    return 0;
}

/* ======================================================================== */
/* X25519 (Curve25519 ECDH, RFC 7748) — TweetNaCl scalarmult (public domain) */
/* GF(2^255-19) elements as 16 signed 16-bit limbs.                          */
/* ======================================================================== */
typedef int64_t tc_gf[16];
static const tc_gf tc_121665 = { 0xDB41, 1 };

static void tc_car(tc_gf o) {
    for (int i = 0; i < 16; i++) {
        o[i] += (int64_t)1 << 16;
        int64_t c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c << 16;
    }
}
static void tc_sel(tc_gf p, tc_gf q, int b) {                 /* constant-time conditional swap */
    int64_t c = ~((int64_t)b - 1);
    for (int i = 0; i < 16; i++) { int64_t t = c & (p[i] ^ q[i]); p[i] ^= t; q[i] ^= t; }
}
static void tc_pack(uint8_t *o, const tc_gf n) {
    tc_gf m, t;
    for (int i = 0; i < 16; i++) t[i] = n[i];
    tc_car(t); tc_car(t); tc_car(t);
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) { m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1); m[i - 1] &= 0xffff; }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int b = (m[15] >> 16) & 1;
        m[14] &= 0xffff;
        tc_sel(t, m, 1 - b);
    }
    for (int i = 0; i < 16; i++) { o[2 * i] = t[i] & 0xff; o[2 * i + 1] = (uint8_t)(t[i] >> 8); }
}
static void tc_unpack(tc_gf o, const uint8_t *n) {
    for (int i = 0; i < 16; i++) o[i] = n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
    o[15] &= 0x7fff;
}
static void tc_gadd(tc_gf o, const tc_gf a, const tc_gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] + b[i]; }
static void tc_gsub(tc_gf o, const tc_gf a, const tc_gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] - b[i]; }
static void tc_gmul(tc_gf o, const tc_gf a, const tc_gf b) {
    int64_t t[31];
    for (int i = 0; i < 31; i++) t[i] = 0;
    for (int i = 0; i < 16; i++) for (int j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    tc_car(o); tc_car(o);
}
static void tc_gsqr(tc_gf o, const tc_gf a) { tc_gmul(o, a, a); }
static void tc_ginv(tc_gf o, const tc_gf in) {                /* o = in^(p-2) mod p */
    tc_gf c;
    for (int a = 0; a < 16; a++) c[a] = in[a];
    for (int a = 253; a >= 0; a--) { tc_gsqr(c, c); if (a != 2 && a != 4) tc_gmul(c, c, in); }
    for (int a = 0; a < 16; a++) o[a] = c[a];
}
static void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]) {
    uint8_t z[32];
    int64_t x[80], r;
    tc_gf a, b, c, d, e, f;
    for (int i = 0; i < 31; i++) z[i] = scalar[i];
    z[31] = (scalar[31] & 127) | 64; z[0] &= 248;             /* clamp */
    tc_unpack(x, point);
    for (int i = 0; i < 16; i++) { b[i] = x[i]; d[i] = a[i] = c[i] = 0; }
    a[0] = d[0] = 1;
    for (int i = 254; i >= 0; i--) {                          /* Montgomery ladder */
        r = (z[i >> 3] >> (i & 7)) & 1;
        tc_sel(a, b, (int)r); tc_sel(c, d, (int)r);
        tc_gadd(e, a, c); tc_gsub(a, a, c); tc_gadd(c, b, d); tc_gsub(b, b, d);
        tc_gsqr(d, e); tc_gsqr(f, a);
        tc_gmul(a, c, a); tc_gmul(c, b, e);
        tc_gadd(e, a, c); tc_gsub(a, a, c); tc_gsqr(b, a); tc_gsub(c, d, f);
        tc_gmul(a, c, tc_121665); tc_gadd(a, a, d); tc_gmul(c, c, a);
        tc_gmul(a, d, f); tc_gmul(d, b, x); tc_gsqr(b, e);
        tc_sel(a, b, (int)r); tc_sel(c, d, (int)r);
    }
    for (int i = 0; i < 16; i++) { x[i + 16] = a[i]; x[i + 32] = c[i]; x[i + 48] = b[i]; x[i + 64] = d[i]; }
    tc_ginv(x + 32, x + 32);
    tc_gmul(x + 16, x + 16, x + 32);
    tc_pack(out, x + 16);
}
static void x25519_base(uint8_t out[32], const uint8_t scalar[32]) {
    static const uint8_t nine[32] = { 9 };
    x25519(out, scalar, nine);
}

/* ======================================================================== */
/* Ed25519 (RFC 8032) — SSH host-key verification + client-auth signatures.  */
/* Edwards-curve arithmetic over the same GF(2^255-19) field ops above       */
/* (adapted from the public-domain TweetNaCl); uses SHA-512.                  */
/* ======================================================================== */
static const tc_gf tc_gf0 = { 0 };
static const tc_gf tc_gf1 = { 1 };
static const tc_gf tc_ed_D  = { 0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070,
                                0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203 };
static const tc_gf tc_ed_D2 = { 0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0,
                                0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406 };
static const tc_gf tc_ed_X  = { 0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c,
                                0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169 };
static const tc_gf tc_ed_Y  = { 0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
                                0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666 };
static const tc_gf tc_ed_I  = { 0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43,
                                0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83 };
static const uint8_t tc_ed_L[32] = {
    0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10 };

static void tc_set(tc_gf o, const tc_gf a) { for (int i = 0; i < 16; i++) o[i] = a[i]; }
static int tc_verify32(const uint8_t *x, const uint8_t *y) {
    unsigned d = 0;
    for (int i = 0; i < 32; i++) d |= (unsigned)(x[i] ^ y[i]);
    return (int)((1u & ((d - 1) >> 8)) - 1u);          /* 0 if equal, -1 otherwise */
}
static int tc_par(const tc_gf a) { uint8_t d[32]; tc_pack(d, a); return d[0] & 1; }
static int tc_neq(const tc_gf a, const tc_gf b) {
    uint8_t c[32], d[32]; tc_pack(c, a); tc_pack(d, b); return tc_verify32(c, d);
}
static void tc_pow2523(tc_gf o, const tc_gf i) {       /* o = i^(2^252 - 3), a sqrt helper */
    tc_gf c; tc_set(c, i);
    for (int a = 250; a >= 0; a--) { tc_gsqr(c, c); if (a != 1) tc_gmul(c, c, i); }
    tc_set(o, c);
}
static void tc_ge_add(tc_gf p[4], tc_gf q[4]) {        /* extended-coordinate point add */
    tc_gf a, b, c, d, t, e, f, g, h;
    tc_gsub(a, p[1], p[0]); tc_gsub(t, q[1], q[0]); tc_gmul(a, a, t);
    tc_gadd(b, p[0], p[1]); tc_gadd(t, q[0], q[1]); tc_gmul(b, b, t);
    tc_gmul(c, p[3], q[3]); tc_gmul(c, c, tc_ed_D2);
    tc_gmul(d, p[2], q[2]); tc_gadd(d, d, d);
    tc_gsub(e, b, a); tc_gsub(f, d, c); tc_gadd(g, d, c); tc_gadd(h, b, a);
    tc_gmul(p[0], e, f); tc_gmul(p[1], h, g); tc_gmul(p[2], g, f); tc_gmul(p[3], e, h);
}
static void tc_ge_cswap(tc_gf p[4], tc_gf q[4], uint8_t b) { for (int i = 0; i < 4; i++) tc_sel(p[i], q[i], b); }
static void tc_ge_pack(uint8_t *r, tc_gf p[4]) {
    tc_gf tx, ty, zi;
    tc_ginv(zi, p[2]); tc_gmul(tx, p[0], zi); tc_gmul(ty, p[1], zi);
    tc_pack(r, ty); r[31] ^= tc_par(tx) << 7;
}
static void tc_ge_scalarmult(tc_gf p[4], tc_gf q[4], const uint8_t *s) {
    tc_set(p[0], tc_gf0); tc_set(p[1], tc_gf1); tc_set(p[2], tc_gf1); tc_set(p[3], tc_gf0);
    for (int i = 255; i >= 0; i--) {
        uint8_t b = (s[i >> 3] >> (i & 7)) & 1;
        tc_ge_cswap(p, q, b); tc_ge_add(q, p); tc_ge_add(p, p); tc_ge_cswap(p, q, b);
    }
}
static void tc_ge_scalarbase(tc_gf p[4], const uint8_t *s) {
    tc_gf q[4];
    tc_set(q[0], tc_ed_X); tc_set(q[1], tc_ed_Y); tc_set(q[2], tc_gf1); tc_gmul(q[3], tc_ed_X, tc_ed_Y);
    tc_ge_scalarmult(p, q, s);
}
static void tc_modL(uint8_t *r, int64_t x[64]) {       /* reduce a 512-bit value mod the group order L */
    int64_t carry; int i, j;
    for (i = 63; i >= 32; --i) {
        carry = 0;
        for (j = i - 32; j < i - 12; ++j) {
            x[j] += carry - 16 * x[i] * tc_ed_L[j - (i - 32)];
            carry = (x[j] + 128) >> 8; x[j] -= carry << 8;
        }
        x[j] += carry; x[i] = 0;
    }
    carry = 0;
    for (j = 0; j < 32; j++) { x[j] += carry - (x[31] >> 4) * tc_ed_L[j]; carry = x[j] >> 8; x[j] &= 255; }
    for (j = 0; j < 32; j++) x[j] -= carry * tc_ed_L[j];
    for (i = 0; i < 32; i++) { x[i + 1] += x[i] >> 8; r[i] = x[i] & 255; }
}
static void tc_reduce(uint8_t *r) {
    int64_t x[64];
    for (int i = 0; i < 64; i++) x[i] = (int64_t)(uint64_t)r[i];
    for (int i = 0; i < 64; i++) r[i] = 0;
    tc_modL(r, x);
}
static int tc_ge_unpackneg(tc_gf r[4], const uint8_t p[32]) {  /* decompress a point (negated for verify) */
    tc_gf t, chk, num, den, den2, den4, den6;
    tc_set(r[2], tc_gf1); tc_unpack(r[1], p);
    tc_gsqr(num, r[1]); tc_gmul(den, num, tc_ed_D); tc_gsub(num, num, r[2]); tc_gadd(den, r[2], den);
    tc_gsqr(den2, den); tc_gsqr(den4, den2); tc_gmul(den6, den4, den2);
    tc_gmul(t, den6, num); tc_gmul(t, t, den);
    tc_pow2523(t, t); tc_gmul(t, t, num); tc_gmul(t, t, den); tc_gmul(t, t, den); tc_gmul(r[0], t, den);
    tc_gsqr(chk, r[0]); tc_gmul(chk, chk, den);
    if (tc_neq(chk, num)) tc_gmul(r[0], r[0], tc_ed_I);
    tc_gsqr(chk, r[0]); tc_gmul(chk, chk, den);
    if (tc_neq(chk, num)) return -1;
    if (tc_par(r[0]) == (p[31] >> 7)) tc_gsub(r[0], tc_gf0, r[0]);
    tc_gmul(r[3], r[0], r[1]);
    return 0;
}

/* Derive the 32-byte public key from a 32-byte seed (private key). */
static void ed25519_pubkey(uint8_t pub[32], const uint8_t seed[32]) {
    uint8_t d[64]; tc_gf p[4];
    sha512(seed, 32, d);
    d[0] &= 248; d[31] &= 127; d[31] |= 64;
    tc_ge_scalarbase(p, d); tc_ge_pack(pub, p);
}

/* Produce a 64-byte detached signature over m using seed + its public key. */
static void ed25519_sign(uint8_t sig[64], const uint8_t *m, size_t n,
                         const uint8_t seed[32], const uint8_t pub[32]) {
    uint8_t d[64], r[64], h[64]; tc_gf p[4]; int64_t x[64];
    sha512_ctx c;
    sha512(seed, 32, d);
    d[0] &= 248; d[31] &= 127; d[31] |= 64;
    sha512_init(&c); sha512_update(&c, d + 32, 32); sha512_update(&c, m, n); sha512_final(&c, r);
    tc_reduce(r);
    tc_ge_scalarbase(p, r); tc_ge_pack(sig, p);         /* R = r*B */
    sha512_init(&c); sha512_update(&c, sig, 32); sha512_update(&c, pub, 32); sha512_update(&c, m, n); sha512_final(&c, h);
    tc_reduce(h);
    for (int i = 0; i < 64; i++) x[i] = 0;
    for (int i = 0; i < 32; i++) x[i] = (int64_t)(uint64_t)r[i];
    for (int i = 0; i < 32; i++) for (int j = 0; j < 32; j++) x[i + j] += (int64_t)h[i] * (int64_t)d[j];
    tc_modL(sig + 32, x);                               /* S = (r + h*a) mod L */
}

/* Verify a 64-byte detached signature; returns 1 if valid, 0 otherwise. */
static int ed25519_verify(const uint8_t sig[64], const uint8_t *m, size_t n, const uint8_t pub[32]) {
    uint8_t h[64], t[32]; tc_gf p[4], q[4];
    if (tc_ge_unpackneg(q, pub)) return 0;
    sha512_ctx c;
    sha512_init(&c); sha512_update(&c, sig, 32); sha512_update(&c, pub, 32); sha512_update(&c, m, n); sha512_final(&c, h);
    tc_reduce(h);
    tc_ge_scalarmult(p, q, h);                          /* h*(-A) */
    tc_ge_scalarbase(q, sig + 32);                      /* S*B */
    tc_ge_add(p, q);                                    /* S*B - h*A */
    tc_ge_pack(t, p);
    return tc_verify32(sig, t) == 0;                    /* R ?= S*B - h*A */
}

#endif /* TCRYPTO_H */
