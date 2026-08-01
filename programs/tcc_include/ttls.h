/* TinyOS userspace TLS 1.3 client — Cycle 9 Phase C4.
 *
 * A minimal RFC 8446 client: X25519 key exchange, TLS_CHACHA20_POLY1305_SHA256,
 * the HKDF-Expand-Label key schedule, and the AEAD record layer.  Built on the
 * primitives in tcrypto.h; talks over a plain socket fd (read/write), so it
 * layers directly on the existing BSD sockets.
 *
 *   !!! SECURITY: this phase does NOT validate the server certificate chain or
 *   the CertificateVerify signature.  The connection is ENCRYPTED but NOT
 *   AUTHENTICATED — it is vulnerable to active man-in-the-middle attacks.
 *   Certificate validation (X.509 + signature verify) is a later phase.  Do not
 *   use for anything security-sensitive yet.  tls_is_authenticated() == 0. */
#ifndef TTLS_H
#define TTLS_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include "tcrypto.h"

#define TLS_REC_MAX 16700

typedef struct {
    int fd;
    uint8_t hs_key_c[32], hs_iv_c[12], hs_key_s[32], hs_iv_s[12];   /* handshake traffic keys */
    uint8_t ap_key_c[32], ap_iv_c[12], ap_key_s[32], ap_iv_s[12];   /* application traffic keys */
    uint8_t hs_secret[32], c_hs_secret[32], s_hs_secret[32];
    uint64_t seq_send, seq_recv;
    sha256_ctx transcript;                                          /* running SHA-256 of handshake msgs */
    uint8_t appbuf[TLS_REC_MAX]; size_t applen, apppos;            /* leftover decrypted app data */
    int authenticated;                                             /* always 0 in this phase */
} tls_conn;

static int tls_is_authenticated(const tls_conn *c) { return c->authenticated; }

/* ---- byte helpers ---- */
static void t_put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void t_put24(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 16); p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)v; }
static uint16_t t_get16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t t_get24(const uint8_t *p) { return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]; }

/* ---- full read/write over the fd ---- */
static int t_read_full(int fd, uint8_t *b, size_t n) {
    size_t got = 0;
    while (got < n) { long r = read(fd, b + got, n - got); if (r <= 0) return (int)got; got += (size_t)r; }
    return (int)got;
}
static int t_write_full(int fd, const uint8_t *b, size_t n) {
    size_t put = 0;
    while (put < n) { long r = write(fd, b + put, n - put); if (r <= 0) return (int)put; put += (size_t)r; }
    return (int)put;
}

/* Fill buf with random bytes.  Uses getrandom(2); falls back to a weak time-seeded
 * PRNG (INSECURE) where that syscall is unavailable (e.g. on-device). */
static void tls_random(uint8_t *buf, size_t len) {
    long r = syscall(318, (long)buf, (long)len, 0L);            /* SYS_getrandom on x86-64 */
    if (r == (long)len) return;
    static uint64_t s = 0;
    if (!s) s = (uint64_t)time(0) ^ 0x9e3779b97f4a7c15ULL;
    s ^= (uint64_t)(uintptr_t)buf;
    for (size_t i = 0; i < len; i++) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; buf[i] = (uint8_t)(s >> 24); }
}

/* ---- HKDF-Expand-Label / Derive-Secret (RFC 8446 §7.1) ---- */
static void tls_expand_label(const uint8_t secret[32], const char *label,
                             const uint8_t *ctx, size_t ctxlen, uint8_t *out, size_t outlen) {
    uint8_t info[2 + 1 + 262 + 1 + 32]; size_t p = 0;
    size_t llen = 6 + strlen(label);                            /* "tls13 " + label */
    t_put16(info + p, (uint16_t)outlen); p += 2;
    info[p++] = (uint8_t)llen;
    memcpy(info + p, "tls13 ", 6); p += 6;
    memcpy(info + p, label, strlen(label)); p += strlen(label);
    info[p++] = (uint8_t)ctxlen;
    if (ctxlen) { memcpy(info + p, ctx, ctxlen); p += ctxlen; }
    hkdf_expand(secret, info, p, out, outlen);
}
/* transcript hash of everything so far (clones the running context) */
static void tls_transcript(tls_conn *c, uint8_t out[32]) { sha256_ctx t = c->transcript; sha256_final(&t, out); }

static void tls_traffic_keys(const uint8_t secret[32], uint8_t key[32], uint8_t iv[12]) {
    tls_expand_label(secret, "key", 0, 0, key, 32);
    tls_expand_label(secret, "iv", 0, 0, iv, 12);
}

/* ---- key schedule ---- */
static void tls_derive_handshake(tls_conn *c, const uint8_t ecdhe[32]) {
    uint8_t zero[32] = { 0 }, early[32], derived[32], ehash[32], thash[32];
    sha256("", 0, ehash);
    hkdf_extract(0, 0, zero, 32, early);                        /* Early Secret */
    tls_expand_label(early, "derived", ehash, 32, derived, 32);
    hkdf_extract(derived, 32, ecdhe, 32, c->hs_secret);        /* Handshake Secret */
    tls_transcript(c, thash);                                   /* CH..SH */
    tls_expand_label(c->hs_secret, "c hs traffic", thash, 32, c->c_hs_secret, 32);
    tls_expand_label(c->hs_secret, "s hs traffic", thash, 32, c->s_hs_secret, 32);
    tls_traffic_keys(c->c_hs_secret, c->hs_key_c, c->hs_iv_c);
    tls_traffic_keys(c->s_hs_secret, c->hs_key_s, c->hs_iv_s);
}
static void tls_derive_application(tls_conn *c) {
    uint8_t zero[32] = { 0 }, derived[32], master[32], ehash[32], thash[32], cas[32], sas[32];
    sha256("", 0, ehash);
    tls_expand_label(c->hs_secret, "derived", ehash, 32, derived, 32);
    hkdf_extract(derived, 32, zero, 32, master);               /* Master Secret */
    tls_transcript(c, thash);                                   /* CH..server Finished */
    tls_expand_label(master, "c ap traffic", thash, 32, cas, 32);
    tls_expand_label(master, "s ap traffic", thash, 32, sas, 32);
    tls_traffic_keys(cas, c->ap_key_c, c->ap_iv_c);
    tls_traffic_keys(sas, c->ap_key_s, c->ap_iv_s);
}

/* ---- record layer ---- */
static void tls_nonce(const uint8_t iv[12], uint64_t seq, uint8_t nonce[12]) {
    memcpy(nonce, iv, 12);
    for (int i = 0; i < 8; i++) nonce[11 - i] ^= (uint8_t)(seq >> (8 * i));
}
/* ep: 0 = plaintext, 1 = handshake keys, 2 = application keys */
static int tls_send_record(tls_conn *c, int ep, int inner_type, const uint8_t *data, size_t len) {
    static uint8_t rec[5 + TLS_REC_MAX];
    if (ep == 0) {
        rec[0] = (uint8_t)inner_type; rec[1] = 0x03; rec[2] = 0x03; t_put16(rec + 3, (uint16_t)len);
        memcpy(rec + 5, data, len);
        return t_write_full(c->fd, rec, 5 + len) == (int)(5 + len) ? 0 : -1;
    }
    size_t ilen = len + 1, clen = ilen + 16;                    /* inner (+type) + tag */
    rec[0] = 23; rec[1] = 0x03; rec[2] = 0x03; t_put16(rec + 3, (uint16_t)clen);
    static uint8_t inner[TLS_REC_MAX];
    memcpy(inner, data, len); inner[len] = (uint8_t)inner_type;
    const uint8_t *key = ep == 1 ? c->hs_key_c : c->ap_key_c;
    const uint8_t *iv  = ep == 1 ? c->hs_iv_c  : c->ap_iv_c;
    uint8_t nonce[12]; tls_nonce(iv, c->seq_send, nonce);
    chacha20poly1305_encrypt(key, nonce, rec, 5, inner, ilen, rec + 5, rec + 5 + ilen);
    c->seq_send++;
    return t_write_full(c->fd, rec, 5 + clen) == (int)(5 + clen) ? 0 : -1;
}
/* Returns 0 and sets *type to the (inner, if encrypted) content type; <0 on error. */
static int tls_recv_record(tls_conn *c, int ep, int *type, uint8_t *out, size_t *outlen) {
    uint8_t hdr[5];
    if (t_read_full(c->fd, hdr, 5) != 5) return -1;
    size_t rlen = t_get16(hdr + 3);
    if (rlen > TLS_REC_MAX) return -1;
    static uint8_t body[TLS_REC_MAX];
    if (t_read_full(c->fd, body, rlen) != (int)rlen) return -1;
    if (ep == 0 || hdr[0] != 23) { *type = hdr[0]; memcpy(out, body, rlen); *outlen = rlen; return 0; }
    if (rlen < 17) return -1;
    size_t ctlen = rlen - 16;
    const uint8_t *key = ep == 1 ? c->hs_key_s : c->ap_key_s;
    const uint8_t *iv  = ep == 1 ? c->hs_iv_s  : c->ap_iv_s;
    uint8_t nonce[12]; tls_nonce(iv, c->seq_recv, nonce);
    if (chacha20poly1305_decrypt(key, nonce, hdr, 5, body, ctlen, body + ctlen, out) != 0) return -2;
    c->seq_recv++;
    size_t p = ctlen;
    while (p > 0 && out[p - 1] == 0) p--;                       /* strip zero padding */
    if (p == 0) return -1;
    *type = out[p - 1]; *outlen = p - 1;
    return 0;
}

/* ---- ClientHello / ServerHello ---- */
static size_t tls_build_client_hello(uint8_t *out, const char *host, const uint8_t pub[32],
                                     const uint8_t random[32], const uint8_t sid[32]) {
    uint8_t *p = out + 4;                                       /* leave room for handshake header */
    size_t hlen = strlen(host);
    *p++ = 0x03; *p++ = 0x03;                                   /* legacy_version */
    memcpy(p, random, 32); p += 32;
    *p++ = 32; memcpy(p, sid, 32); p += 32;                     /* legacy_session_id (compat) */
    *p++ = 0x00; *p++ = 0x02; *p++ = 0x13; *p++ = 0x03;        /* cipher_suites: TLS_CHACHA20_POLY1305_SHA256 */
    *p++ = 0x01; *p++ = 0x00;                                   /* compression: null */
    uint8_t *extlen_at = p; p += 2;
    /* supported_versions -> TLS 1.3 */
    t_put16(p, 0x002b); t_put16(p + 2, 3); p[4] = 2; t_put16(p + 5, 0x0304); p += 7;
    /* supported_groups -> x25519 */
    t_put16(p, 0x000a); t_put16(p + 2, 4); t_put16(p + 4, 2); t_put16(p + 6, 0x001d); p += 8;
    /* signature_algorithms (advertised; not validated this phase) */
    t_put16(p, 0x000d); t_put16(p + 2, 10); t_put16(p + 4, 8);
    t_put16(p + 6, 0x0403); t_put16(p + 8, 0x0804); t_put16(p + 10, 0x0401); t_put16(p + 12, 0x0807); p += 14;
    /* key_share -> x25519 public */
    t_put16(p, 0x0033); t_put16(p + 2, 38); t_put16(p + 4, 36);
    t_put16(p + 6, 0x001d); t_put16(p + 8, 32); memcpy(p + 10, pub, 32); p += 42;
    /* server_name (SNI) */
    t_put16(p, 0x0000); t_put16(p + 2, (uint16_t)(hlen + 5)); t_put16(p + 4, (uint16_t)(hlen + 3));
    p[6] = 0x00; t_put16(p + 7, (uint16_t)hlen); memcpy(p + 9, host, hlen); p += 9 + hlen;
    t_put16(extlen_at, (uint16_t)(p - (extlen_at + 2)));
    size_t bodylen = (size_t)(p - (out + 4));
    out[0] = 1; t_put24(out + 1, (uint32_t)bodylen);           /* handshake header: client_hello */
    return 4 + bodylen;
}
static int tls_parse_server_hello(const uint8_t *m, size_t mlen, uint8_t server_pub[32]) {
    static const uint8_t hrr[32] = {                           /* HelloRetryRequest magic random */
        0xcf,0x21,0xad,0x74,0xe5,0x9a,0x61,0x11,0xbe,0x1d,0x8c,0x02,0x1e,0x65,0xb8,0x91,
        0xc2,0xa2,0x11,0x16,0x7a,0xbb,0x8c,0x5e,0x07,0x9e,0x09,0xe2,0xc8,0xa8,0x33,0x9c };
    if (mlen < 4 || m[0] != 2) return -1;
    size_t bl = t_get24(m + 1);
    if (bl + 4 > mlen) return -1;
    const uint8_t *p = m + 4, *end = m + 4 + bl;
    if (p + 35 > end) return -1;
    p += 2;                                                     /* legacy_version */
    if (memcmp(p, hrr, 32) == 0) return -3;                     /* HelloRetryRequest unsupported */
    p += 32;                                                    /* random */
    uint8_t sidlen = *p++; p += sidlen;                        /* session id echo */
    if (p + 3 > end) return -1;
    if (t_get16(p) != 0x1303) return -1;                       /* must be ChaCha20-Poly1305 */
    p += 3;                                                     /* cipher suite + compression */
    if (p + 2 > end) return -1;
    uint16_t extlen = t_get16(p); p += 2;
    const uint8_t *ee = p + extlen; if (ee > end) ee = end;
    while (p + 4 <= ee) {
        uint16_t et = t_get16(p), el = t_get16(p + 2); p += 4;
        if (p + el > ee) break;
        if (et == 0x0033 && el >= 36 && t_get16(p) == 0x001d && t_get16(p + 2) == 32) {
            memcpy(server_pub, p + 4, 32); return 0;
        }
        p += el;
    }
    return -1;
}

/* Read the server's encrypted handshake flight (EE, Certificate, CertVerify,
 * Finished), feeding each message into the transcript.  Returns 0 at Finished. */
static int tls_read_server_flight(tls_conn *c) {
    static uint8_t acc[65536]; size_t acclen = 0, pos = 0;
    for (;;) {
        while (acclen - pos >= 4) {
            uint32_t msglen = 4 + t_get24(acc + pos + 1);
            if (acclen - pos < msglen) break;
            int htype = acc[pos];
            sha256_update(&c->transcript, acc + pos, msglen);
            pos += msglen;
            if (htype == 20) return 0;                          /* server Finished */
        }
        if (pos > 0) { memmove(acc, acc + pos, acclen - pos); acclen -= pos; pos = 0; }
        int type; static uint8_t rec[TLS_REC_MAX]; size_t reclen;
        int r = tls_recv_record(c, 1, &type, rec, &reclen);
        if (r != 0) return -1;
        if (type == 21) return -1;                              /* alert */
        if (type != 22) continue;                               /* skip CCS / others */
        if (acclen + reclen > sizeof acc) return -1;
        memcpy(acc + acclen, rec, reclen); acclen += reclen;
    }
}

/* Perform the full TLS 1.3 client handshake on socket fd for `host` (SNI).
 * Returns 0 on success.  NOTE: does not authenticate the server (see header). */
static int tls_client_handshake(tls_conn *c, int fd, const char *host) {
    memset(c, 0, sizeof *c);
    c->fd = fd;
    sha256_init(&c->transcript);

    uint8_t priv[32], pub[32], random[32], sid[32];
    tls_random(priv, 32); tls_random(random, 32); tls_random(sid, 32);
    x25519_base(pub, priv);

    static uint8_t ch[1024];
    size_t chlen = tls_build_client_hello(ch, host, pub, random, sid);
    sha256_update(&c->transcript, ch, chlen);
    if (tls_send_record(c, 0, 22, ch, chlen) != 0) return -1;

    int type; static uint8_t rec[TLS_REC_MAX]; size_t reclen;
    if (tls_recv_record(c, 0, &type, rec, &reclen) != 0) return -1;
    if (type != 22) return -1;
    sha256_update(&c->transcript, rec, reclen);
    uint8_t server_pub[32];
    if (tls_parse_server_hello(rec, reclen, server_pub) != 0) return -2;

    uint8_t ecdhe[32]; x25519(ecdhe, priv, server_pub);
    tls_derive_handshake(c, ecdhe);
    c->seq_recv = 0;

    uint8_t ccs = 1; tls_send_record(c, 0, 20, &ccs, 1);       /* middlebox-compat CCS */

    if (tls_read_server_flight(c) != 0) return -3;             /* transcript now CH..server Finished */

    uint8_t fin_key[32], thash[32], verify[32];
    tls_expand_label(c->c_hs_secret, "finished", 0, 0, fin_key, 32);
    tls_transcript(c, thash);
    hmac_sha256(fin_key, 32, thash, 32, verify);
    tls_derive_application(c);                                  /* app secrets use CH..server Finished */

    uint8_t finmsg[36]; finmsg[0] = 20; t_put24(finmsg + 1, 32); memcpy(finmsg + 4, verify, 32);
    c->seq_send = 0;
    if (tls_send_record(c, 1, 22, finmsg, sizeof finmsg) != 0) return -1;  /* client Finished */

    c->seq_send = 0; c->seq_recv = 0;                          /* application epoch */
    c->applen = c->apppos = 0;
    c->authenticated = 0;
    return 0;
}

/* ---- application data ---- */
static int tls_write(tls_conn *c, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data; size_t left = len;
    while (left) {
        size_t n = left > 16384 ? 16384 : left;
        if (tls_send_record(c, 2, 23, p, n) != 0) return -1;
        p += n; left -= n;
    }
    return (int)len;
}
/* Returns bytes read (>0), 0 at end/close_notify, <0 on error. */
static int tls_read(tls_conn *c, void *buf, size_t max) {
    while (c->apppos == c->applen) {
        int type; size_t reclen;
        int r = tls_recv_record(c, 2, &type, c->appbuf, &reclen);
        if (r != 0) return r == -2 ? -1 : 0;
        if (type == 23) { c->applen = reclen; c->apppos = 0; }
        else if (type == 21) return 0;                          /* alert: close_notify -> EOF */
        /* type 22 (NewSessionTicket etc.) / 20 (CCS): ignore, read next */
    }
    size_t avail = c->applen - c->apppos, n = avail > max ? max : avail;
    memcpy(buf, c->appbuf + c->apppos, n); c->apppos += n;
    return (int)n;
}
static void tls_close(tls_conn *c) {
    uint8_t alert[2] = { 1, 0 };                                /* warning, close_notify */
    tls_send_record(c, 2, 21, alert, 2);
    close(c->fd);
}

#endif /* TTLS_H */
