/* TinyOS userspace SSH transport — Cycle 9 C7b (git over SSH).
 *
 * Implements the SSH 2.0 transport layer (RFC 4253) far enough to clone over
 * ssh://: version exchange, the binary packet protocol, algorithm negotiation
 * (KEXINIT), curve25519-sha256 key exchange (RFC 8731), ssh-ed25519 host-key
 * signature verification, key derivation, and the chacha20-poly1305@openssh.com
 * AEAD cipher.  The userauth + channel layers live in tssh_auth.h / git.c.
 *
 * Depends on tcrypto.h (X25519, Ed25519, SHA-256, ChaCha20, Poly1305).  Plain C
 * for on-device TinyCC.  The ssh_conn struct is large (per-packet buffers) and
 * MUST be static or heap-allocated, never placed on the small user stack.
 *
 * SECURITY: the caller is responsible for checking the server host key against
 * a known-hosts value (ssh_conn.hostkey / .ks) — signature verification only
 * proves the peer holds that key, not that it is the host you intended. */
#ifndef TSSH_H
#define TSSH_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include "tcrypto.h"

/* ---- SSH message numbers ---- */
#define SSH_MSG_DISCONNECT       1
#define SSH_MSG_IGNORE           2
#define SSH_MSG_UNIMPLEMENTED    3
#define SSH_MSG_DEBUG            4
#define SSH_MSG_SERVICE_REQUEST  5
#define SSH_MSG_SERVICE_ACCEPT   6
#define SSH_MSG_KEXINIT          20
#define SSH_MSG_NEWKEYS          21
#define SSH_MSG_KEX_ECDH_INIT    30
#define SSH_MSG_KEX_ECDH_REPLY   31
#define SSH_MSG_USERAUTH_REQUEST 50
#define SSH_MSG_USERAUTH_FAILURE 51
#define SSH_MSG_USERAUTH_SUCCESS 52
#define SSH_MSG_USERAUTH_BANNER  53
#define SSH_MSG_USERAUTH_PK_OK   60
#define SSH_MSG_GLOBAL_REQUEST   80
#define SSH_MSG_REQUEST_SUCCESS  81
#define SSH_MSG_REQUEST_FAILURE  82
#define SSH_MSG_CHANNEL_OPEN                 90
#define SSH_MSG_CHANNEL_OPEN_CONFIRMATION    91
#define SSH_MSG_CHANNEL_OPEN_FAILURE         92
#define SSH_MSG_CHANNEL_WINDOW_ADJUST        93
#define SSH_MSG_CHANNEL_DATA                 94
#define SSH_MSG_CHANNEL_EXTENDED_DATA        95
#define SSH_MSG_CHANNEL_EOF                  96
#define SSH_MSG_CHANNEL_CLOSE                97
#define SSH_MSG_CHANNEL_REQUEST              98
#define SSH_MSG_CHANNEL_SUCCESS              99
#define SSH_MSG_CHANNEL_FAILURE              100

#define SSH_PKT_MAX 65536

typedef struct {
    int fd;
    uint32_t send_seq, recv_seq;
    int encrypted;
    uint8_t kc2s[64], ks2c[64];       /* chacha20-poly1305 keys (K_2 || K_1)  */
    uint8_t session_id[32];
    char v_c[64], v_s[256];           /* version strings, no CR/LF            */
    uint8_t ic[2048]; size_t ic_len;  /* our KEXINIT payload  (I_C)           */
    uint8_t is[8192]; size_t is_len;  /* server KEXINIT payload (I_S)         */
    uint8_t ks[1024]; size_t ks_len;  /* server host-key blob (K_S)           */
    uint8_t hostkey[32];              /* parsed ed25519 host public key       */
    uint32_t local_chan, remote_chan;
    uint32_t local_window, remote_window, remote_maxpkt;
    int chan_closed;
    uint8_t pkt[SSH_PKT_MAX + 64];    /* one decrypted incoming payload       */
    uint8_t wire[SSH_PKT_MAX + 256];  /* one outgoing framed packet           */
} ssh_conn;

/* ------------------------------------------------------------------ */
/* low-level I/O + randomness                                         */
/* ------------------------------------------------------------------ */
static int ssh_read_full(int fd, void *buf, size_t n) {
    uint8_t *p = (uint8_t *)buf; size_t got = 0;
    while (got < n) { long r = read(fd, p + got, n - got); if (r <= 0) return -1; got += (size_t)r; }
    return 0;
}
static int ssh_write_all(int fd, const void *buf, size_t n) {
    const uint8_t *p = (const uint8_t *)buf; size_t s = 0;
    while (s < n) { long r = write(fd, p + s, n - s); if (r <= 0) return -1; s += (size_t)r; }
    return 0;
}
static void ssh_random(uint8_t *b, size_t n) {
    long r = syscall(318, (long)b, (long)n, 0L);          /* getrandom */
    if (r == (long)n) return;
    static uint64_t s = 0;
    if (!s) s = (uint64_t)time(0) ^ 0x9e3779b97f4a7c15ULL;
    for (size_t i = 0; i < n; i++) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; b[i] = (uint8_t)s; }
}

/* ------------------------------------------------------------------ */
/* SSH wire encoding helpers                                          */
/* ------------------------------------------------------------------ */
static size_t ssh_put_u32(uint8_t *b, size_t p, uint32_t v) {
    b[p] = (uint8_t)(v >> 24); b[p+1] = (uint8_t)(v >> 16); b[p+2] = (uint8_t)(v >> 8); b[p+3] = (uint8_t)v; return p + 4;
}
static size_t ssh_put_string(uint8_t *b, size_t p, const void *d, size_t n) {
    p = ssh_put_u32(b, p, (uint32_t)n); memcpy(b + p, d, n); return p + n;
}
static size_t ssh_put_cstr(uint8_t *b, size_t p, const char *s) { return ssh_put_string(b, p, s, strlen(s)); }

static int ssh_get_u32(const uint8_t *b, size_t len, size_t *p, uint32_t *v) {
    if (*p + 4 > len) return -1;
    *v = ((uint32_t)b[*p] << 24) | ((uint32_t)b[*p+1] << 16) | ((uint32_t)b[*p+2] << 8) | b[*p+3];
    *p += 4; return 0;
}
static int ssh_get_string(const uint8_t *b, size_t len, size_t *p, const uint8_t **s, uint32_t *sl) {
    uint32_t n; if (ssh_get_u32(b, len, p, &n)) return -1;
    if (*p + n > len) return -1;
    *s = b + *p; *sl = n; *p += n; return 0;
}

static void ssh_hash_u32(sha256_ctx *h, uint32_t v) {
    uint8_t b[4] = { (uint8_t)(v>>24), (uint8_t)(v>>16), (uint8_t)(v>>8), (uint8_t)v }; sha256_update(h, b, 4);
}
static void ssh_hash_string(sha256_ctx *h, const void *d, size_t n) { ssh_hash_u32(h, (uint32_t)n); sha256_update(h, d, n); }
static void ssh_hash_mpint(sha256_ctx *h, const uint8_t *x, size_t n) {
    size_t i = 0; while (i < n && x[i] == 0) i++;
    size_t len = n - i; int pad = (len > 0 && (x[i] & 0x80)) ? 1 : 0;
    ssh_hash_u32(h, (uint32_t)(len + pad));
    if (pad) { uint8_t z = 0; sha256_update(h, &z, 1); }
    if (len) sha256_update(h, x + i, len);
}

/* ------------------------------------------------------------------ */
/* chacha20-poly1305@openssh.com packet layer                        */
/* ------------------------------------------------------------------ */
static void ssh_chacha_nonce(uint8_t nonce[12], uint32_t seq) {
    memset(nonce, 0, 12);                                  /* 64-bit BE seq in the low 8 bytes */
    nonce[8] = (uint8_t)(seq >> 24); nonce[9] = (uint8_t)(seq >> 16);
    nonce[10] = (uint8_t)(seq >> 8); nonce[11] = (uint8_t)seq;
}

/* Frame, optionally encrypt, and send one payload. */
static int ssh_send_payload(ssh_conn *c, const uint8_t *payload, size_t plen) {
    uint8_t *w = c->wire;
    if (!c->encrypted) {
        size_t pad = 8 - ((5 + plen) % 8); if (pad < 4) pad += 8;
        uint32_t pktlen = (uint32_t)(1 + plen + pad);
        size_t p = ssh_put_u32(w, 0, pktlen);
        w[p++] = (uint8_t)pad;
        memcpy(w + p, payload, plen); p += plen;
        ssh_random(w + p, pad); p += pad;
        c->send_seq++;
        return ssh_write_all(c->fd, w, p);
    }
    size_t pad = 8 - ((1 + plen) % 8); if (pad < 4) pad += 8;
    uint32_t pktlen = (uint32_t)(1 + plen + pad);
    uint8_t nonce[12]; ssh_chacha_nonce(nonce, c->send_seq);
    uint8_t lenbe[4]; ssh_put_u32(lenbe, 0, pktlen);
    chacha20_xor(c->kc2s + 32, 0, nonce, lenbe, w, 4);     /* K_1 encrypts the length */
    uint8_t *pt = w + 4;
    pt[0] = (uint8_t)pad;
    memcpy(pt + 1, payload, plen);
    ssh_random(pt + 1 + plen, pad);
    uint32_t st[16]; uint8_t polykey[64];
    chacha20_setup(st, c->kc2s, 0, nonce); chacha20_block(st, polykey);   /* K_2 ctr 0 -> Poly1305 key */
    chacha20_xor(c->kc2s, 1, nonce, pt, pt, pktlen);       /* K_2 ctr 1.. -> payload */
    uint8_t tag[16]; poly1305(polykey, w, 4 + pktlen, tag);
    memcpy(w + 4 + pktlen, tag, 16);
    c->send_seq++;
    return ssh_write_all(c->fd, w, 4 + pktlen + 16);
}

/* Receive (and decrypt) one payload; skips IGNORE/DEBUG, fails on DISCONNECT.
 * Returns a pointer into c->pkt with the payload and sets *outlen, or NULL. */
static uint8_t *ssh_recv_payload(ssh_conn *c, size_t *outlen) {
    for (;;) {
        uint32_t pktlen;
        if (!c->encrypted) {
            uint8_t lb[4]; if (ssh_read_full(c->fd, lb, 4)) return NULL;
            pktlen = ((uint32_t)lb[0] << 24) | ((uint32_t)lb[1] << 16) | ((uint32_t)lb[2] << 8) | lb[3];
            if (pktlen < 2 || pktlen > SSH_PKT_MAX) return NULL;
            if (ssh_read_full(c->fd, c->pkt, pktlen)) return NULL;
            c->recv_seq++;
        } else {
            uint8_t lb[4]; if (ssh_read_full(c->fd, lb, 4)) return NULL;
            uint8_t nonce[12]; ssh_chacha_nonce(nonce, c->recv_seq);
            uint8_t lenbe[4]; chacha20_xor(c->ks2c + 32, 0, nonce, lb, lenbe, 4);
            pktlen = ((uint32_t)lenbe[0] << 24) | ((uint32_t)lenbe[1] << 16) | ((uint32_t)lenbe[2] << 8) | lenbe[3];
            if (pktlen < 2 || pktlen > SSH_PKT_MAX) return NULL;
            if (ssh_read_full(c->fd, c->pkt, pktlen)) return NULL;
            uint8_t tag[16]; if (ssh_read_full(c->fd, tag, 16)) return NULL;
            uint32_t st[16]; uint8_t polykey[64];
            chacha20_setup(st, c->ks2c, 0, nonce); chacha20_block(st, polykey);
            uint8_t calc[16]; poly1305_ctx ps; poly1305_init(&ps, polykey);
            poly1305_update(&ps, lb, 4); poly1305_update(&ps, c->pkt, pktlen); poly1305_finish(&ps, calc);
            uint8_t d = 0; for (int i = 0; i < 16; i++) d |= calc[i] ^ tag[i];
            if (d) return NULL;                            /* authentication failure */
            c->recv_seq++;
            chacha20_xor(c->ks2c, 1, nonce, c->pkt, c->pkt, pktlen);
        }
        uint8_t padlen = c->pkt[0];
        if ((size_t)padlen + 1 > pktlen) return NULL;
        size_t plen = pktlen - padlen - 1;
        uint8_t msg = plen ? c->pkt[1] : 0;
        if (msg == SSH_MSG_IGNORE || msg == SSH_MSG_DEBUG || msg == SSH_MSG_UNIMPLEMENTED) continue;
        if (msg == SSH_MSG_DISCONNECT) return NULL;
        memmove(c->pkt, c->pkt + 1, plen);                 /* drop padding_length byte */
        *outlen = plen;
        return c->pkt;
    }
}

/* ------------------------------------------------------------------ */
/* version exchange + key exchange                                    */
/* ------------------------------------------------------------------ */
static int ssh_exchange_versions(ssh_conn *c) {
    strcpy(c->v_c, "SSH-2.0-tinyos_git_1.0");
    char line[300];
    size_t vl = strlen(c->v_c);
    memcpy(line, c->v_c, vl); line[vl] = '\r'; line[vl + 1] = '\n';
    if (ssh_write_all(c->fd, line, vl + 2)) return -1;
    for (int tries = 0; tries < 50; tries++) {             /* banner lines may precede the version */
        size_t i = 0;
        while (i < sizeof(line) - 1) {
            char ch; long r = read(c->fd, &ch, 1); if (r <= 0) return -1;
            if (ch == '\n') break;
            if (ch != '\r') line[i++] = ch;
        }
        line[i] = 0;
        if (!strncmp(line, "SSH-", 4)) { strncpy(c->v_s, line, sizeof c->v_s - 1); c->v_s[sizeof c->v_s - 1] = 0; return 0; }
    }
    return -1;
}

/* Derive `outlen` bytes of key material for a given letter (RFC 4253 §7.2). */
static void ssh_derive(ssh_conn *c, char letter, uint8_t *out, size_t outlen, const uint8_t *K, const uint8_t *H) {
    uint8_t block[32]; size_t done = 0;
    sha256_ctx s; sha256_init(&s);
    ssh_hash_mpint(&s, K, 32); sha256_update(&s, H, 32);
    uint8_t l = (uint8_t)letter; sha256_update(&s, &l, 1);
    sha256_update(&s, c->session_id, 32);
    sha256_final(&s, block);
    size_t take = outlen < 32 ? outlen : 32; memcpy(out, block, take); done = take;
    while (done < outlen) {
        sha256_init(&s);
        ssh_hash_mpint(&s, K, 32); sha256_update(&s, H, 32); sha256_update(&s, out, done);
        sha256_final(&s, block);
        take = outlen - done; if (take > 32) take = 32;
        memcpy(out + done, block, take); done += take;
    }
}

/* Run the full transport handshake; 0 on success (encryption then active). */
static int ssh_handshake(ssh_conn *c) {
    if (ssh_exchange_versions(c)) return -1;

    /* --- our KEXINIT --- */
    size_t p = 0;
    c->ic[p++] = SSH_MSG_KEXINIT;
    ssh_random(c->ic + p, 16); p += 16;
    p = ssh_put_cstr(c->ic, p, "curve25519-sha256,curve25519-sha256@libssh.org");
    p = ssh_put_cstr(c->ic, p, "ssh-ed25519");
    p = ssh_put_cstr(c->ic, p, "chacha20-poly1305@openssh.com");
    p = ssh_put_cstr(c->ic, p, "chacha20-poly1305@openssh.com");
    p = ssh_put_cstr(c->ic, p, "hmac-sha2-256");
    p = ssh_put_cstr(c->ic, p, "hmac-sha2-256");
    p = ssh_put_cstr(c->ic, p, "none");
    p = ssh_put_cstr(c->ic, p, "none");
    p = ssh_put_cstr(c->ic, p, "");
    p = ssh_put_cstr(c->ic, p, "");
    c->ic[p++] = 0;                                        /* first_kex_packet_follows */
    p = ssh_put_u32(c->ic, p, 0);                          /* reserved */
    c->ic_len = p;
    if (ssh_send_payload(c, c->ic, c->ic_len)) return -1;

    /* --- server KEXINIT --- */
    size_t sl;
    uint8_t *sp = ssh_recv_payload(c, &sl);
    if (!sp || sp[0] != SSH_MSG_KEXINIT || sl > sizeof c->is) return -1;
    memcpy(c->is, sp, sl); c->is_len = sl;

    /* --- KEX_ECDH_INIT --- */
    uint8_t priv[32], qc[32];
    ssh_random(priv, 32);
    x25519_base(qc, priv);
    uint8_t m[64]; size_t mp = 0;
    m[mp++] = SSH_MSG_KEX_ECDH_INIT;
    mp = ssh_put_string(m, mp, qc, 32);
    if (ssh_send_payload(c, m, mp)) return -1;

    /* --- KEX_ECDH_REPLY --- */
    sp = ssh_recv_payload(c, &sl);
    if (!sp || sp[0] != SSH_MSG_KEX_ECDH_REPLY) return -1;
    size_t q = 1;
    const uint8_t *ks, *qs, *sig; uint32_t kslen, qslen, siglen;
    if (ssh_get_string(sp, sl, &q, &ks, &kslen)) return -1;
    if (ssh_get_string(sp, sl, &q, &qs, &qslen)) return -1;
    if (ssh_get_string(sp, sl, &q, &sig, &siglen)) return -1;
    if (qslen != 32 || kslen > sizeof c->ks) return -1;
    memcpy(c->ks, ks, kslen); c->ks_len = kslen;

    /* shared secret */
    uint8_t K[32]; x25519(K, priv, qs);

    /* exchange hash H */
    uint8_t H[32];
    sha256_ctx hc; sha256_init(&hc);
    ssh_hash_string(&hc, c->v_c, strlen(c->v_c));
    ssh_hash_string(&hc, c->v_s, strlen(c->v_s));
    ssh_hash_string(&hc, c->ic, c->ic_len);
    ssh_hash_string(&hc, c->is, c->is_len);
    ssh_hash_string(&hc, c->ks, c->ks_len);
    ssh_hash_string(&hc, qc, 32);
    ssh_hash_string(&hc, qs, 32);
    ssh_hash_mpint(&hc, K, 32);
    sha256_final(&hc, H);

    /* parse host key blob (ssh-ed25519 string + 32-byte key) */
    size_t kp = 0; const uint8_t *kt, *kk; uint32_t ktl, kkl;
    if (ssh_get_string(c->ks, c->ks_len, &kp, &kt, &ktl)) return -1;
    if (ktl != 11 || memcmp(kt, "ssh-ed25519", 11) != 0) return -1;
    if (ssh_get_string(c->ks, c->ks_len, &kp, &kk, &kkl)) return -1;
    if (kkl != 32) return -1;
    memcpy(c->hostkey, kk, 32);

    /* parse signature blob (ssh-ed25519 string + 64-byte sig) */
    size_t sgp = 0; const uint8_t *st_, *sg; uint32_t stl, sgl;
    if (ssh_get_string(sig, siglen, &sgp, &st_, &stl)) return -1;
    if (stl != 11 || memcmp(st_, "ssh-ed25519", 11) != 0) return -1;
    if (ssh_get_string(sig, siglen, &sgp, &sg, &sgl)) return -1;
    if (sgl != 64) return -1;

    if (!ed25519_verify(sg, H, 32, c->hostkey)) return -1; /* host authenticated the exchange */

    /* session id = H (first kex); derive the directional keys */
    memcpy(c->session_id, H, 32);
    ssh_derive(c, 'C', c->kc2s, 64, K, H);
    ssh_derive(c, 'D', c->ks2c, 64, K, H);

    /* NEWKEYS both ways, then encryption is live */
    uint8_t nk = SSH_MSG_NEWKEYS;
    if (ssh_send_payload(c, &nk, 1)) return -1;
    sp = ssh_recv_payload(c, &sl);
    if (!sp || sp[0] != SSH_MSG_NEWKEYS) return -1;
    c->encrypted = 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* userauth (RFC 4252) — publickey with an ed25519 key                */
/* ------------------------------------------------------------------ */
/* Build the blob signed for publickey auth: string(session_id) || the
 * USERAUTH_REQUEST with the "signature present" flag set.  Returns its length;
 * the request itself (for the wire) is sd + 36 (past the session_id string). */
static size_t ssh_userauth_signdata(uint8_t *sd, const uint8_t sid[32], const char *user, const uint8_t *pub) {
    uint8_t pkblob[128]; size_t kp = 0;
    kp = ssh_put_cstr(pkblob, kp, "ssh-ed25519");
    kp = ssh_put_string(pkblob, kp, pub, 32);
    size_t sp = 0;
    sp = ssh_put_string(sd, sp, sid, 32);
    sd[sp++] = SSH_MSG_USERAUTH_REQUEST;
    sp = ssh_put_cstr(sd, sp, user);
    sp = ssh_put_cstr(sd, sp, "ssh-connection");
    sp = ssh_put_cstr(sd, sp, "publickey");
    sd[sp++] = 1;                                    /* signature present */
    sp = ssh_put_cstr(sd, sp, "ssh-ed25519");
    sp = ssh_put_string(sd, sp, pkblob, kp);
    return sp;
}

/* Returns 0 on success, 1 if the server rejected the key, -1 on error. */
static int ssh_userauth_publickey(ssh_conn *c, const char *user,
                                  const uint8_t seed[32], const uint8_t pub[32]) {
    uint8_t req[64]; size_t p = 0;
    req[p++] = SSH_MSG_SERVICE_REQUEST;
    p = ssh_put_cstr(req, p, "ssh-userauth");
    if (ssh_send_payload(c, req, p)) return -1;
    size_t l; uint8_t *r = ssh_recv_payload(c, &l);
    if (!r || r[0] != SSH_MSG_SERVICE_ACCEPT) return -1;

    uint8_t sd[512];
    size_t sp = ssh_userauth_signdata(sd, c->session_id, user, pub);
    uint8_t sig[64]; ed25519_sign(sig, sd, sp, seed, pub);
    uint8_t sigblob[128]; size_t gp = 0;
    gp = ssh_put_cstr(sigblob, gp, "ssh-ed25519");
    gp = ssh_put_string(sigblob, gp, sig, 64);

    uint8_t out[512]; size_t op = sp - 36;           /* request bytes follow the session_id string */
    memcpy(out, sd + 36, op);
    op = ssh_put_string(out, op, sigblob, gp);
    if (ssh_send_payload(c, out, op)) return -1;

    for (;;) {
        r = ssh_recv_payload(c, &l);
        if (!r) return -1;
        if (r[0] == SSH_MSG_USERAUTH_BANNER) continue;
        if (r[0] == SSH_MSG_USERAUTH_SUCCESS) return 0;
        if (r[0] == SSH_MSG_USERAUTH_FAILURE) return 1;
        return -1;
    }
}

/* ------------------------------------------------------------------ */
/* connection channels (RFC 4254)                                     */
/* ------------------------------------------------------------------ */
#define SSH_WINDOW 0x200000u

static int ssh_channel_open(ssh_conn *c) {
    c->local_chan = 0; c->local_window = SSH_WINDOW; c->chan_closed = 0;
    uint8_t m[64]; size_t p = 0;
    m[p++] = SSH_MSG_CHANNEL_OPEN;
    p = ssh_put_cstr(m, p, "session");
    p = ssh_put_u32(m, p, c->local_chan);
    p = ssh_put_u32(m, p, c->local_window);
    p = ssh_put_u32(m, p, 32768);
    if (ssh_send_payload(c, m, p)) return -1;
    for (;;) {
        size_t l; uint8_t *r = ssh_recv_payload(c, &l);
        if (!r) return -1;
        if (r[0] == SSH_MSG_CHANNEL_OPEN_CONFIRMATION) {
            size_t q = 1; uint32_t rc, sc, rw, rmax;
            if (ssh_get_u32(r, l, &q, &rc)) return -1;
            if (ssh_get_u32(r, l, &q, &sc)) return -1;
            if (ssh_get_u32(r, l, &q, &rw)) return -1;
            if (ssh_get_u32(r, l, &q, &rmax)) return -1;
            c->remote_chan = sc; c->remote_window = rw; c->remote_maxpkt = rmax ? rmax : 32768;
            return 0;
        }
        if (r[0] == SSH_MSG_CHANNEL_OPEN_FAILURE) return -1;
        if (r[0] == SSH_MSG_GLOBAL_REQUEST) {
            size_t q = 1; const uint8_t *nm; uint32_t nl;
            if (!ssh_get_string(r, l, &q, &nm, &nl) && q < l && r[q]) { uint8_t rf = SSH_MSG_REQUEST_FAILURE; ssh_send_payload(c, &rf, 1); }
        }
    }
}

static int ssh_channel_exec(ssh_conn *c, const char *cmd) {
    uint8_t m[512]; size_t p = 0;
    m[p++] = SSH_MSG_CHANNEL_REQUEST;
    p = ssh_put_u32(m, p, c->remote_chan);
    p = ssh_put_cstr(m, p, "exec");
    m[p++] = 0;                                      /* want_reply = FALSE (data follows) */
    p = ssh_put_cstr(m, p, cmd);
    return ssh_send_payload(c, m, p);
}

/* Receive one chunk of channel data; returns bytes (into out), 0 at EOF/close,
 * -1 on error.  Replenishes our receive window and drains control messages. */
static long ssh_channel_recv(ssh_conn *c, uint8_t *out, size_t outcap) {
    for (;;) {
        if (c->chan_closed) return 0;
        size_t l; uint8_t *r = ssh_recv_payload(c, &l);
        if (!r) return -1;
        uint8_t t = r[0];
        if (t == SSH_MSG_CHANNEL_DATA) {
            size_t q = 1; uint32_t ch; const uint8_t *d; uint32_t dl;
            if (ssh_get_u32(r, l, &q, &ch)) return -1;
            if (ssh_get_string(r, l, &q, &d, &dl)) return -1;
            if (dl > outcap) return -1;
            memcpy(out, d, dl);
            if (c->local_window > dl) c->local_window -= dl; else c->local_window = 0;
            if (c->local_window < SSH_WINDOW / 2) {   /* top the window back up */
                uint8_t wm[16]; size_t wp = 0;
                wm[wp++] = SSH_MSG_CHANNEL_WINDOW_ADJUST;
                wp = ssh_put_u32(wm, wp, c->remote_chan);
                wp = ssh_put_u32(wm, wp, SSH_WINDOW);
                if (ssh_send_payload(c, wm, wp)) return -1;
                c->local_window += SSH_WINDOW;
            }
            return (long)dl;
        }
        if (t == SSH_MSG_CHANNEL_EXTENDED_DATA) {     /* stderr — count against window, discard */
            size_t q = 1; uint32_t ch, code; const uint8_t *d; uint32_t dl;
            ssh_get_u32(r, l, &q, &ch); ssh_get_u32(r, l, &q, &code);
            if (!ssh_get_string(r, l, &q, &d, &dl)) { if (c->local_window > dl) c->local_window -= dl; }
            continue;
        }
        if (t == SSH_MSG_CHANNEL_WINDOW_ADJUST) {
            size_t q = 1; uint32_t ch, inc; ssh_get_u32(r, l, &q, &ch);
            if (!ssh_get_u32(r, l, &q, &inc)) c->remote_window += inc;
            continue;
        }
        if (t == SSH_MSG_CHANNEL_EOF) continue;
        if (t == SSH_MSG_CHANNEL_CLOSE) { c->chan_closed = 1; return 0; }
        if (t == SSH_MSG_CHANNEL_SUCCESS || t == SSH_MSG_CHANNEL_FAILURE) continue;
        if (t == SSH_MSG_GLOBAL_REQUEST) {
            size_t q = 1; const uint8_t *nm; uint32_t nl;
            if (!ssh_get_string(r, l, &q, &nm, &nl) && q < l && r[q]) { uint8_t rf = SSH_MSG_REQUEST_FAILURE; ssh_send_payload(c, &rf, 1); }
            continue;
        }
        /* ignore anything else */
    }
}

/* Send channel data, honouring the peer's window and max packet size. */
static int ssh_channel_write(ssh_conn *c, const uint8_t *data, size_t len) {
    size_t off = 0;
    uint8_t frame[16384];
    while (off < len) {
        size_t chunk = len - off;
        if (chunk > c->remote_maxpkt) chunk = c->remote_maxpkt;
        if (chunk > sizeof frame - 16) chunk = sizeof frame - 16;
        while (c->remote_window < chunk) {            /* wait for a window adjust */
            size_t l; uint8_t *r = ssh_recv_payload(c, &l);
            if (!r) return -1;
            if (r[0] == SSH_MSG_CHANNEL_WINDOW_ADJUST) {
                size_t q = 1; uint32_t ch, inc; ssh_get_u32(r, l, &q, &ch);
                if (!ssh_get_u32(r, l, &q, &inc)) c->remote_window += inc;
            } else if (r[0] == SSH_MSG_CHANNEL_CLOSE) { c->chan_closed = 1; return -1; }
        }
        size_t p = 0;
        frame[p++] = SSH_MSG_CHANNEL_DATA;
        p = ssh_put_u32(frame, p, c->remote_chan);
        p = ssh_put_string(frame, p, data + off, chunk);
        if (ssh_send_payload(c, frame, p)) return -1;
        c->remote_window -= chunk;
        off += chunk;
    }
    return 0;
}

#endif /* TSSH_H */
