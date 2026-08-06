/* SPDX-License-Identifier: GPL-2.0-only */

/* git — a minimal git clone/ls-remote client over HTTP(S) for TinyOS (Cycle 9 C6).
 *
 * Speaks the git smart-HTTP protocol: it discovers refs with
 *   GET  <repo>/info/refs?service=git-upload-pack
 * and fetches objects with
 *   POST <repo>/git-upload-pack
 * decoding the chunked HTTP body, the pkt-line framing and the side-band-64k
 * multiplexing to recover the packfile, which tgit.h then parses and resolves.
 * A working tree is written out from the HEAD commit.
 *
 *   git ls-remote <url>              list the remote refs
 *   git clone [--depth N] <url> [dir]   clone into dir
 *
 * https:// uses the TLS 1.3 client (ttls.h); http:// uses a plain socket.
 * Build on device:  ./tcc.bin /git.c -o /git.bin
 *
 * SECURITY: for https, the server certificate is NOT verified yet (see ttls.h)
 * — transport is encrypted but not authenticated.  A warning is printed. */
#include <tnet.h>
#include <ttls.h>
#include <tgit.h>
#include <tssh.h>
#include <tdeflate.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* growable byte buffer                                               */
/* ------------------------------------------------------------------ */
typedef struct { uint8_t *p; size_t len, cap; } buf;

static int buf_add(buf *b, const void *d, size_t n) {
    if (b->len + n > b->cap) {
        size_t nc = b->cap ? b->cap : 8192;
        while (nc < b->len + n) nc *= 2;
        uint8_t *np = (uint8_t *)realloc(b->p, nc);
        if (!np) return -1;
        b->p = np; b->cap = nc;
    }
    memcpy(b->p + b->len, d, n);
    b->len += n;
    return 0;
}
static void buf_free(buf *b) { free(b->p); b->p = NULL; b->len = b->cap = 0; }

static void die(const char *msg) { write(2, msg, strlen(msg)); write(2, "\n", 1); }

/* case-insensitive substring search (avoids a <strings.h> dependency). */
static int ci_contains(const uint8_t *hay, size_t n, const char *needle) {
    size_t m = strlen(needle);
    if (m == 0 || m > n) return 0;
    for (size_t i = 0; i + m <= n; i++) {
        size_t k = 0;
        for (; k < m; k++) {
            int a = hay[i + k], b = needle[k];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
        }
        if (k == m) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* one HTTP(S) request/response over a fresh connection               */
/* ------------------------------------------------------------------ */
static tls_conn g_tls;      /* ~17KB record buffer — must stay off the stack */

static int resolve_host(const char *host, uint32_t *ip) {
    uint32_t lit = ntohl(inet_addr(host));
    *ip = (lit != 0xFFFFFFFFu && lit != 0) ? lit : net_resolve(host);
    return *ip ? 0 : -1;
}

/* Send `method path` with optional body, collect the whole raw response
 * (status line + headers + body) into *resp.  Returns 0 on success. */
static int http_fetch(const char *host, int port, int use_tls,
                      const char *method, const char *path, const char *ctype,
                      const uint8_t *body, size_t blen, buf *resp) {
    uint32_t ip;
    if (resolve_host(host, &ip) != 0) { die("git: cannot resolve host"); return -1; }

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { die("git: socket failed"); return -1; }
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((unsigned short)port); a.sin_addr.s_addr = htonl(ip);
    if (connect(s, (struct sockaddr *)&a, sizeof a) != 0) { die("git: connect failed"); close(s); return -1; }

    tls_conn *tc = NULL;
    if (use_tls) {
        tc = &g_tls;
        int r = tls_client_handshake(tc, s, host);
        if (r != 0) { die("git: TLS handshake failed"); close(s); return -1; }
        if (!tls_is_authenticated(tc))
            die("git: WARNING: server certificate not verified (encrypted, not authenticated)");
    }

    char hdr[1024];
    int hl = snprintf(hdr, sizeof hdr,
        "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: tinyos-git/1.0\r\n"
        "Accept: */*\r\n%s%s%s"
        "Connection: close\r\n",
        method, path, host,
        ctype ? "Content-Type: " : "", ctype ? ctype : "", ctype ? "\r\n" : "");
    char clen[64]; clen[0] = 0;
    if (body) snprintf(clen, sizeof clen, "Content-Length: %zu\r\n", blen);

    #define SEND(d, n) do { int _n = (n); const char *_d = (const char *)(d); \
        int w = tc ? tls_write(tc, _d, _n) : (int)write(s, _d, _n); \
        if (w != _n) { die("git: send failed"); if (tc) tls_close(tc); close(s); return -1; } } while (0)
    SEND(hdr, hl);
    if (clen[0]) SEND(clen, (int)strlen(clen));
    SEND("\r\n", 2);
    if (body && blen) SEND(body, (int)blen);
    #undef SEND

    uint8_t rb[4096];
    for (;;) {
        int n = tc ? tls_read(tc, rb, sizeof rb) : (int)read(s, rb, sizeof rb);
        if (n <= 0) break;
        if (buf_add(resp, rb, n) != 0) { die("git: out of memory"); if (tc) tls_close(tc); close(s); return -1; }
    }
    if (tc) tls_close(tc);
    close(s);
    return 0;
}

/* ------------------------------------------------------------------ */
/* HTTP response body: split headers, dechunk                         */
/* ------------------------------------------------------------------ */
/* Returns 0 and fills *out (a freshly-allocated dechunked body) on 2xx. */
static int http_body(const buf *resp, buf *out) {
    const uint8_t *p = resp->p;
    size_t n = resp->len;
    if (n < 12 || memcmp(p, "HTTP/1.", 7) != 0) { die("git: not an HTTP response"); return -1; }
    int status = atoi((const char *)p + 9);

    const uint8_t *hdr_end = NULL;
    for (size_t i = 0; i + 3 < n; i++)
        if (p[i] == '\r' && p[i+1] == '\n' && p[i+2] == '\r' && p[i+3] == '\n') { hdr_end = p + i + 4; break; }
    if (!hdr_end) { die("git: malformed HTTP headers"); return -1; }

    int chunked = ci_contains(p, (size_t)(hdr_end - p), "transfer-encoding: chunked");

    if (status / 100 != 2) {
        char m[64]; snprintf(m, sizeof m, "git: server returned HTTP %d", status);
        die(m);
        return -1;
    }

    const uint8_t *bp = hdr_end;
    size_t blen = n - (hdr_end - p);
    if (!chunked) return buf_add(out, bp, blen) == 0 ? 0 : -1;

    size_t i = 0;                                    /* dechunk */
    while (i < blen) {
        size_t j = i;
        while (j < blen && bp[j] != '\r') j++;
        if (j + 1 >= blen) break;
        long csz = strtol((const char *)bp + i, NULL, 16);
        i = j + 2;                                    /* skip CRLF after size */
        if (csz <= 0) break;                          /* last chunk */
        if (i + (size_t)csz > blen) return -1;
        if (buf_add(out, bp + i, csz) != 0) return -1;
        i += csz + 2;                                 /* data + trailing CRLF */
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* side-band-64k demux: band 1 = pack, 2 = progress, 3 = error        */
/* ------------------------------------------------------------------ */
static int sideband(const uint8_t *d, size_t len, buf *pack) {
    size_t p = 0;
    for (;;) {
        const uint8_t *pl;
        long n = git_pkt_next(d, len, &p, &pl);
        if (n < 0) break;                             /* end / malformed -> stop */
        if (n == 0) continue;                         /* flush between sections */
        if (pl[0] == 1) {                             /* pack data */
            if (buf_add(pack, pl + 1, n - 1) != 0) return -1;
        } else if (pl[0] == 3) {                      /* fatal error from server */
            die("git: remote error");
            write(2, (const char *)pl + 1, n - 1); write(2, "\n", 1);
            return -1;
        } else if (pl[0] == 2) {
            /* progress — ignored */
        }
        /* control lines (NAK/ACK/shallow, first byte >= 0x20) are skipped */
    }
    return pack->len ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* URL parsing                                                        */
/* ------------------------------------------------------------------ */
typedef struct { char host[256]; int port; int tls; int ssh; char user[64]; char path[512]; } url_t;

static void append_git(char *path, size_t cap) {
    size_t pl = strlen(path);
    if (!(pl >= 4 && strcmp(path + pl - 4, ".git") == 0) && pl + 4 < cap) strcpy(path + pl, ".git");
}

static int url_parse(const char *u, url_t *o) {
    memset(o, 0, sizeof *o);
    strcpy(o->user, "git");
    if (!strncmp(u, "https://", 8)) { u += 8; o->tls = 1; o->port = 443; }
    else if (!strncmp(u, "http://", 7)) { u += 7; o->tls = 0; o->port = 80; }
    else if (!strncmp(u, "ssh://", 6)) { u += 6; o->ssh = 1; o->port = 22; }
    else {
        /* scp-like  [user@]host:path  (a colon that precedes any slash) */
        const char *colon = strchr(u, ':'), *slash = strchr(u, '/');
        if (colon && (!slash || colon < slash)) {
            o->ssh = 1; o->port = 22;
            const char *at = strchr(u, '@');
            int i = 0;
            if (at && at < colon) { while (u < at && i < 63) o->user[i++] = *u++; o->user[i] = 0; u++; }
            i = 0; while (*u && *u != ':' && i < 255) o->host[i++] = *u++; o->host[i] = 0;
            if (*u == ':') u++;
            strncpy(o->path, u, sizeof o->path - 1); o->path[sizeof o->path - 1] = 0;
            append_git(o->path, sizeof o->path);
            return o->host[0] ? 0 : -1;
        }
        die("git: URL must be http(s)://, ssh://, or user@host:path"); return -1;
    }
    const char *at = strchr(u, '@'), *slash = strchr(u, '/');
    if (o->ssh && at && (!slash || at < slash)) { int i = 0; while (u < at && i < 63) o->user[i++] = *u++; o->user[i] = 0; u++; }
    int i = 0;
    while (*u && *u != '/' && *u != ':' && i < 255) o->host[i++] = *u++;
    o->host[i] = 0;
    if (*u == ':') { o->port = atoi(u + 1); while (*u && *u != '/') u++; }
    const char *path = (*u == '/') ? u : "/";
    strncpy(o->path, path, sizeof o->path - 1); o->path[sizeof o->path - 1] = 0;
    append_git(o->path, sizeof o->path);
    return o->host[0] ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* ref discovery                                                      */
/* ------------------------------------------------------------------ */
typedef struct { char sha[41]; char name[256]; } ref_t;

/* Parse a git ref advertisement (pkt-lines) into refs[]; returns the count and
 * copies the first ref's capability list into caps.  Works for both the HTTP
 * advertisement (leading "# service" pkt + flush) and the bare SSH one. */
static int parse_refs(const uint8_t *body, size_t blen, ref_t *refs, int max, char *caps, size_t capsz) {
    size_t p = 0; int count = 0, first = 1; caps[0] = 0;
    const uint8_t *pl;
    for (;;) {
        long n = git_pkt_next(body, blen, &p, &pl);
        if (n < 0) break;
        if (n == 0) { if (count) break; else continue; }   /* flush ends the ref list */
        if (n >= 1 && pl[0] == '#') continue;               /* "# service=..." */
        if (n >= 4 && !memcmp(pl, "ERR ", 4)) { die("git: remote error on ref discovery"); break; }
        if (n < 40) continue;
        if (count < max) {
            memcpy(refs[count].sha, pl, 40); refs[count].sha[40] = 0;
            size_t k = 41, j = 0;
            while (k < (size_t)n && pl[k] != '\n' && pl[k] != 0 && j < 255) refs[count].name[j++] = pl[k++];
            refs[count].name[j] = 0;
            if (first) {                                    /* caps follow a NUL on the first ref */
                for (size_t z = 0; z + 1 < (size_t)n; z++)
                    if (pl[z] == 0) { size_t c = 0; z++; while (z < (size_t)n && pl[z] != '\n' && c + 1 < capsz) caps[c++] = pl[z++]; caps[c] = 0; break; }
            }
            count++;
        }
        first = 0;
    }
    return count;
}

/* GET info/refs, parse the advertisement.  Returns ref count or -1. */
static int discover(const url_t *u, ref_t *refs, int max, char *caps, size_t capsz) {
    char path[600];
    snprintf(path, sizeof path, "%s/info/refs?service=git-upload-pack", u->path);
    buf resp = {0}, body = {0};
    if (http_fetch(u->host, u->port, u->tls, "GET", path, NULL, NULL, 0, &resp) != 0) { buf_free(&resp); return -1; }
    if (http_body(&resp, &body) != 0) { buf_free(&resp); buf_free(&body); return -1; }
    buf_free(&resp);
    int count = parse_refs(body.p, body.len, refs, max, caps, capsz);
    buf_free(&body);
    return count;
}

/* ------------------------------------------------------------------ */
/* fetch pack for a wanted sha                                        */
/* ------------------------------------------------------------------ */
/* Build the upload-pack request body (want/deepen/flush/done) as pkt-lines. */
static void build_want(buf *req, const char *want_sha, int depth) {
    char line[128]; uint8_t pkt[160];
    int ll = snprintf(line, sizeof line, "want %s side-band-64k ofs-delta\n", want_sha);
    buf_add(req, pkt, git_pkt_put(pkt, line, ll));
    if (depth > 0) { ll = snprintf(line, sizeof line, "deepen %d\n", depth); buf_add(req, pkt, git_pkt_put(pkt, line, ll)); }
    buf_add(req, "0000", 4);
    buf_add(req, pkt, git_pkt_put(pkt, "done\n", 5));
}

static int fetch_pack(const url_t *u, const char *want_sha, int depth, buf *pack) {
    char path[600];
    snprintf(path, sizeof path, "%s/git-upload-pack", u->path);
    buf req = {0};
    build_want(&req, want_sha, depth);
    buf resp = {0}, body = {0};
    int rc = http_fetch(u->host, u->port, u->tls, "POST", path,
                        "application/x-git-upload-pack-request", req.p, req.len, &resp);
    buf_free(&req);
    if (rc != 0) { buf_free(&resp); return -1; }
    if (http_body(&resp, &body) != 0) { buf_free(&resp); buf_free(&body); return -1; }
    buf_free(&resp);
    rc = sideband(body.p, body.len, pack);
    buf_free(&body);
    return rc;
}

/* ------------------------------------------------------------------ */
/* checkout                                                           */
/* ------------------------------------------------------------------ */
static int checkout_tree(gpack *gp, const uint8_t tree_sha[20], const char *dir) {
    gobj *t = git_find(gp, tree_sha);
    if (!t || t->type != GIT_TREE) { die("git: tree object missing"); return -1; }
    gtree_iter it; git_tree_begin(&it, t->data, t->len);
    unsigned mode; const char *name; const uint8_t *esha;
    char path[1024];
    while (git_tree_next(&it, &mode, &name, &esha)) {
        snprintf(path, sizeof path, "%s/%s", dir, name);
        if (mode == 040000) {                          /* subdirectory */
            mkdir(path, 0755);
            if (checkout_tree(gp, esha, path) != 0) return -1;
        } else if (mode == 0120000 || mode == 0160000) {
            char c[41]; git_sha_hex(esha, c);           /* symlink / submodule: skip */
            char m[300]; snprintf(m, sizeof m, "git: skipping %s (mode %06o)", name, mode);
            die(m);
        } else {                                        /* regular file */
            gobj *b = git_find(gp, esha);
            if (!b) { die("git: blob missing"); continue; }
            int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, (mode & 0111) ? 0755 : 0644);
            if (fd < 0) { die("git: cannot create file"); continue; }
            size_t off = 0;
            while (off < b->len) {
                int w = write(fd, b->data + off, b->len - off);
                if (w <= 0) break;
                off += w;
            }
            close(fd);
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* ssh transport (git@host:repo / ssh://)                             */
/* ------------------------------------------------------------------ */
static ssh_conn g_ssh;                                  /* large — kept out of the stack */
static uint8_t g_chunk[65536];                          /* shared channel read buffer */
static const char *g_keypath = "id_ed25519";

static int b64val(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
static int b64decode(const char *in, size_t inlen, uint8_t *out, size_t cap, size_t *outlen) {
    size_t o = 0; int acc = 0, bits = 0;
    for (size_t i = 0; i < inlen; i++) {
        int v = b64val((unsigned char)in[i]); if (v < 0) continue;
        acc = (acc << 6) | v; bits += 6;
        if (bits >= 8) { bits -= 8; if (o >= cap) return -1; out[o++] = (uint8_t)(acc >> bits); }
    }
    *outlen = o; return 0;
}
static void b64encode(const uint8_t *in, int n, char *out) {
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int o = 0;
    for (int i = 0; i < n; i += 3) {
        int r = n - i, v = in[i] << 16 | (r > 1 ? in[i+1] << 8 : 0) | (r > 2 ? in[i+2] : 0);
        out[o++] = t[(v>>18)&63]; out[o++] = t[(v>>12)&63];
        out[o++] = r > 1 ? t[(v>>6)&63] : '='; out[o++] = r > 2 ? t[v&63] : '=';
    }
    out[o] = 0;
}

/* Load an ed25519 private key: a raw 32/64-byte seed, or an unencrypted OpenSSH
 * "-----BEGIN OPENSSH PRIVATE KEY-----" file.  0 on success. */
static int load_ed25519_key(const char *path, uint8_t seed[32], uint8_t pub[32]) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { die("git: cannot open SSH key (use -i <path>)"); return -1; }
    static uint8_t fb[8192];
    int n = (int)read(fd, fb, sizeof fb - 1); close(fd);
    if (n <= 0) return -1;
    fb[n] = 0;
    if (n == 32 || n == 64) { memcpy(seed, fb, 32); ed25519_pubkey(pub, seed); return 0; }
    char *b = strstr((char *)fb, "-----BEGIN OPENSSH PRIVATE KEY-----");
    char *e = b ? strstr(b, "-----END") : NULL;
    if (!b || !e) { die("git: unrecognized SSH key format"); return -1; }
    b += strlen("-----BEGIN OPENSSH PRIVATE KEY-----");
    static uint8_t der[8192]; size_t dl;
    if (b64decode(b, (size_t)(e - b), der, sizeof der, &dl)) return -1;
    size_t p = 0; const uint8_t *s; uint32_t sl;
    if (dl < 15 || memcmp(der, "openssh-key-v1", 15) != 0) return -1;
    p = 15;
    if (ssh_get_string(der, dl, &p, &s, &sl)) return -1;                    /* ciphername */
    if (sl != 4 || memcmp(s, "none", 4) != 0) { die("git: encrypted SSH keys unsupported"); return -1; }
    if (ssh_get_string(der, dl, &p, &s, &sl)) return -1;                    /* kdfname */
    if (ssh_get_string(der, dl, &p, &s, &sl)) return -1;                    /* kdfoptions */
    uint32_t nk; if (ssh_get_u32(der, dl, &p, &nk)) return -1;
    if (ssh_get_string(der, dl, &p, &s, &sl)) return -1;                    /* public blob */
    const uint8_t *priv; uint32_t plen;
    if (ssh_get_string(der, dl, &p, &priv, &plen)) return -1;               /* private section */
    size_t q = 0; uint32_t c1, c2;
    if (ssh_get_u32(priv, plen, &q, &c1) || ssh_get_u32(priv, plen, &q, &c2) || c1 != c2) { die("git: bad or locked SSH key"); return -1; }
    if (ssh_get_string(priv, plen, &q, &s, &sl)) return -1;                 /* keytype */
    if (sl != 11 || memcmp(s, "ssh-ed25519", 11) != 0) { die("git: only ssh-ed25519 keys supported"); return -1; }
    if (ssh_get_string(priv, plen, &q, &s, &sl)) return -1;                 /* public 32 */
    const uint8_t *sk; uint32_t skl;
    if (ssh_get_string(priv, plen, &q, &sk, &skl) || skl != 64) return -1;  /* seed || pub */
    memcpy(seed, sk, 32); memcpy(pub, sk + 32, 32);
    return 0;
}

/* Open a git-upload-pack session over SSH; leaves the channel ready to read. */
static int ssh_start(const url_t *u) {
    uint8_t seed[32], pub[32];
    if (load_ed25519_key(g_keypath, seed, pub) != 0) return -1;

    uint32_t ip;
    if (resolve_host(u->host, &ip) != 0) { die("git: cannot resolve host"); return -1; }
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { die("git: socket failed"); return -1; }
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((unsigned short)u->port); a.sin_addr.s_addr = htonl(ip);
    if (connect(s, (struct sockaddr *)&a, sizeof a) != 0) { die("git: connect failed"); close(s); return -1; }

    memset(&g_ssh, 0, sizeof g_ssh);
    g_ssh.fd = s;
    if (ssh_handshake(&g_ssh) != 0) { die("git: SSH handshake failed"); close(s); return -1; }

    uint8_t fp[32]; sha256(g_ssh.ks, g_ssh.ks_len, fp);
    char fpb[64]; b64encode(fp, 32, fpb); char *eq = strchr(fpb, '='); if (eq) *eq = 0;
    printf("ssh host key SHA256:%s (not verified against known_hosts)\n", fpb);

    int ar = ssh_userauth_publickey(&g_ssh, u->user, seed, pub);
    if (ar != 0) { die(ar > 0 ? "git: SSH auth failed (key not accepted by server)" : "git: SSH auth error"); close(s); return -1; }

    if (ssh_channel_open(&g_ssh) != 0) { die("git: SSH channel open failed"); close(s); return -1; }
    char cmd[600];
    const char *rp = u->path; if (rp[0] == '/') rp++;
    snprintf(cmd, sizeof cmd, "git-upload-pack '%s'", rp);
    if (ssh_channel_exec(&g_ssh, cmd) != 0) { die("git: SSH exec failed"); close(s); return -1; }
    return 0;
}

/* Read the ref advertisement from the SSH channel (up to the terminating flush). */
static int ssh_read_refs(ref_t *refs, int max, char *caps, size_t capsz) {
    buf adv = {0};
    for (;;) {
        size_t p = 0; const uint8_t *pl; int done = 0;
        for (;;) { long n = git_pkt_next(adv.p, adv.len, &p, &pl); if (n < 0) break; if (n == 0) { done = 1; break; } }
        if (done) break;
        long n = ssh_channel_recv(&g_ssh, g_chunk, sizeof g_chunk);
        if (n < 0) { buf_free(&adv); return -1; }
        if (n == 0) break;
        if (buf_add(&adv, g_chunk, (size_t)n)) { buf_free(&adv); return -1; }
    }
    int c = parse_refs(adv.p, adv.len, refs, max, caps, capsz);
    buf_free(&adv);
    return c;
}

/* Send the want list, then read the side-band packfile off the channel. */
static int ssh_fetch(const char *want_sha, int depth, buf *pack) {
    buf req = {0}; build_want(&req, want_sha, depth);
    int rc = ssh_channel_write(&g_ssh, req.p, req.len);
    buf_free(&req);
    if (rc != 0) return -1;
    buf raw = {0};
    for (;;) {
        long n = ssh_channel_recv(&g_ssh, g_chunk, sizeof g_chunk);
        if (n < 0) { buf_free(&raw); return -1; }
        if (n == 0) break;
        if (buf_add(&raw, g_chunk, (size_t)n)) { buf_free(&raw); return -1; }
    }
    rc = sideband(raw.p, raw.len, pack);
    buf_free(&raw);
    return rc;
}

/* ================================================================== */
/* local repository: object store, refs, porcelain                    */
/* ================================================================== */
static int read_whole(const char *path, uint8_t **out, size_t *len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    buf b = {0}; uint8_t tmp[8192]; int n;
    while ((n = read(fd, tmp, sizeof tmp)) > 0) if (buf_add(&b, tmp, n)) { close(fd); buf_free(&b); return -1; }
    close(fd);
    *out = b.p ? b.p : (uint8_t *)calloc(1, 1); *len = b.len;
    return 0;
}
static int write_whole(const char *path, const void *data, size_t len, int mode) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) return -1;
    const uint8_t *p = (const uint8_t *)data; size_t off = 0;
    while (off < len) { int w = write(fd, p + off, len - off); if (w <= 0) { close(fd); return -1; } off += w; }
    close(fd);
    return 0;
}
static void mkdir_p(const char *path) {
    char t[256]; size_t n = strlen(path); if (n >= sizeof t) return;
    memcpy(t, path, n + 1);
    for (size_t i = 1; i < n; i++) if (t[i] == '/') { t[i] = 0; mkdir(t, 0755); t[i] = '/'; }
    mkdir(t, 0755);
}

/* "type len\0" object header; returns its length including the NUL. */
static int git_obj_hdr(int type, size_t len, char *out) {
    const char *ts = git_typestr(type);
    int n = 0; while (*ts) out[n++] = *ts++;
    out[n++] = ' ';
    char num[24]; int nn = 0; size_t v = len;
    if (!v) num[nn++] = '0';
    while (v) { num[nn++] = (char)('0' + v % 10); v /= 10; }
    while (nn) out[n++] = num[--nn];
    out[n++] = 0;
    return n;
}
static void obj_path(const uint8_t sha[20], char *out, size_t cap) {
    char hex[41]; git_sha_hex(sha, hex);
    snprintf(out, cap, ".git/objects/%c%c/%s", hex[0], hex[1], hex + 2);
}

/* Write a loose object (zlib), computing its id.  Idempotent; 0 on success. */
static int obj_write(int type, const uint8_t *data, size_t len, uint8_t sha[20]) {
    char hdr[32]; int hn = git_obj_hdr(type, len, hdr);
    size_t objlen = (size_t)hn + len;
    uint8_t *obj = (uint8_t *)malloc(objlen ? objlen : 1);
    if (!obj) return -1;
    memcpy(obj, hdr, hn); memcpy(obj + hn, data, len);
    sha1(obj, objlen, sha);
    char path[128]; obj_path(sha, path, sizeof path);
    int fd = open(path, O_RDONLY);
    if (fd >= 0) { close(fd); free(obj); return 0; }           /* already stored */
    char dir[64], hex[41]; git_sha_hex(sha, hex);
    snprintf(dir, sizeof dir, ".git/objects/%c%c", hex[0], hex[1]);
    mkdir(dir, 0755);
    size_t zcap = objlen + objlen / 2 + 128;
    uint8_t *z = (uint8_t *)malloc(zcap);
    if (!z) { free(obj); return -1; }
    long zl = tdfl_zlib(obj, objlen, z, zcap);
    int rc = (zl < 0) ? -1 : write_whole(path, z, (size_t)zl, 0444);
    free(obj); free(z);
    return rc;
}

/* Inflate a zlib stream of unknown output size (grow-and-retry). */
static int inflate_grow(const uint8_t *z, size_t zl, uint8_t **out, size_t *outlen) {
    size_t cap = 65536;
    for (;;) {
        uint8_t *d = (uint8_t *)malloc(cap);
        if (!d) return -1;
        size_t used = 0; long n = tinfl_zlib(z, zl, d, cap, &used);
        if (n >= 0) { *out = d; *outlen = (size_t)n; return 0; }
        free(d);
        if (cap > (256u << 20)) return -1;
        cap *= 2;
    }
}

/* Read a loose object; *data is malloc'd content (caller frees). */
static int obj_read(const uint8_t sha[20], int *type, uint8_t **data, size_t *len) {
    char path[128]; obj_path(sha, path, sizeof path);
    uint8_t *z; size_t zl;
    if (read_whole(path, &z, &zl) != 0) return -1;
    uint8_t *ob; size_t obl;
    int rc = inflate_grow(z, zl, &ob, &obl); free(z);
    if (rc != 0) return -1;
    uint8_t *sp = (uint8_t *)memchr(ob, ' ', obl);
    uint8_t *nul = sp ? (uint8_t *)memchr(sp, 0, obl - (sp - ob)) : NULL;
    if (!sp || !nul) { free(ob); return -1; }
    int t = -1; size_t tl = sp - ob;
    if (tl == 6 && !memcmp(ob, "commit", 6)) t = GIT_COMMIT;
    else if (tl == 4 && !memcmp(ob, "tree", 4)) t = GIT_TREE;
    else if (tl == 4 && !memcmp(ob, "blob", 4)) t = GIT_BLOB;
    else if (tl == 3 && !memcmp(ob, "tag", 3)) t = GIT_TAG;
    if (t < 0) { free(ob); return -1; }
    size_t clen = obl - (size_t)(nul + 1 - ob);
    uint8_t *c = (uint8_t *)malloc(clen ? clen : 1);
    if (!c) { free(ob); return -1; }
    memcpy(c, nul + 1, clen); free(ob);
    *type = t; *data = c; *len = clen;
    return 0;
}

/* ---- refs / HEAD ---- */
static int head_ref(char *out, size_t cap) {              /* symref target, e.g. refs/heads/main */
    uint8_t *d; size_t n;
    if (read_whole(".git/HEAD", &d, &n) != 0) return -1;
    int rc = -1;
    if (n > 5 && !memcmp(d, "ref: ", 5)) {
        size_t i = 5, j = 0;
        while (i < n && d[i] != '\n' && d[i] != '\r' && j < cap - 1) out[j++] = (char)d[i++];
        out[j] = 0; rc = 0;
    }
    free(d); return rc;
}
static int ref_read(const char *ref, uint8_t sha[20]) {
    char path[288]; snprintf(path, sizeof path, ".git/%s", ref);
    uint8_t *d; size_t n;
    if (read_whole(path, &d, &n) != 0) return -1;
    int rc = (n >= 40 && git_hex_sha((char *)d, sha) == 0) ? 0 : -1;
    free(d); return rc;
}
static int head_sha(uint8_t sha[20]) {
    char ref[160];
    if (head_ref(ref, sizeof ref) == 0) return ref_read(ref, sha);
    uint8_t *d; size_t n;                                 /* detached HEAD holds a raw sha */
    if (read_whole(".git/HEAD", &d, &n)) return -1;
    int rc = (n >= 40 && git_hex_sha((char *)d, sha) == 0) ? 0 : -1;
    free(d); return rc;
}
static int ref_write(const char *ref, const uint8_t sha[20]) {
    char path[288]; snprintf(path, sizeof path, ".git/%s", ref);
    char sub[288]; snprintf(sub, sizeof sub, ".git/%s", ref);
    for (int i = (int)strlen(sub) - 1; i > 0; i--) if (sub[i] == '/') { sub[i] = 0; mkdir_p(sub); break; }
    char hex[41]; git_sha_hex(sha, hex);
    char line[42]; memcpy(line, hex, 40); line[40] = '\n';
    return write_whole(path, line, 41, 0644);
}
static int repo_ok(void) {
    uint8_t *d; size_t n;
    if (read_whole(".git/HEAD", &d, &n) != 0) { die("git: not a git repository (no .git)"); return 0; }
    free(d); return 1;
}

/* ---- init / hash-object / cat-file ---- */
static int cmd_init(const char *dir) {
    if (dir && dir[0]) { mkdir(dir, 0755); if (chdir(dir) != 0) { die("git: cannot enter target dir"); return 1; } }
    mkdir(".git", 0755);
    mkdir(".git/objects", 0755); mkdir(".git/objects/pack", 0755);
    mkdir(".git/refs", 0755); mkdir(".git/refs/heads", 0755); mkdir(".git/refs/tags", 0755);
    write_whole(".git/HEAD", "ref: refs/heads/main\n", 21, 0644);
    static const char cfg[] = "[core]\n\trepositoryformatversion = 0\n\tfilemode = false\n\tbare = false\n";
    write_whole(".git/config", cfg, sizeof cfg - 1, 0644);
    printf("Initialized empty Git repository in %s.git/\n", dir && dir[0] ? dir : "");
    return 0;
}
static int cmd_hash_object(int argc, char **argv) {
    int wr = 0; const char *file = NULL;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-w")) wr = 1;
        else if (argv[i][0] != '-') file = argv[i];
    }
    if (!file) { printf("usage: git hash-object [-w] <file>\n"); return 1; }
    uint8_t *data; size_t len;
    if (read_whole(file, &data, &len) != 0) { die("git: cannot read file"); return 1; }
    uint8_t sha[20];
    int rc = wr ? obj_write(GIT_BLOB, data, len, sha) : (git_oid(GIT_BLOB, data, len, sha), 0);
    free(data);
    if (rc != 0) { die("git: cannot write object"); return 1; }
    char hex[41]; git_sha_hex(sha, hex); printf("%s\n", hex);
    return 0;
}
static int cmd_cat_file(const char *opt, const char *shahex) {
    if (!repo_ok()) return 1;
    uint8_t sha[20];
    if (strlen(shahex) < 40 || git_hex_sha(shahex, sha) != 0) { die("git: bad object name"); return 1; }
    int type; uint8_t *data; size_t len;
    if (obj_read(sha, &type, &data, &len) != 0) { die("git: object not found"); return 1; }
    if (!strcmp(opt, "-t")) printf("%s\n", git_typestr(type));
    else if (!strcmp(opt, "-s")) printf("%zu\n", len);
    else if (!strcmp(opt, "-p")) {
        if (type == GIT_TREE) {
            gtree_iter it; git_tree_begin(&it, data, len);
            unsigned mode; const char *name; const uint8_t *esha;
            while (git_tree_next(&it, &mode, &name, &esha)) {
                char eh[41]; git_sha_hex(esha, eh);
                const char *tt = (mode == 040000) ? "tree" : (mode == 0160000 ? "commit" : "blob");
                printf("%06o %s %s\t%s\n", mode, tt, eh, name);
            }
        } else write(1, data, len);
    } else { printf("usage: git cat-file (-t|-s|-p) <sha>\n"); free(data); return 1; }
    free(data);
    return 0;
}

/* ------------------------------------------------------------------ */
/* the index (.git/index v2) + add / write-tree / commit / log        */
/* ------------------------------------------------------------------ */
typedef struct {
    uint32_t ctime_s, ctime_n, mtime_s, mtime_n, dev, ino, mode, uid, gid, size;
    uint8_t sha[20];
    char name[256];
} idx_ent;
typedef struct { idx_ent *e; int n, cap; } index_t;

static void put32be(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v>>24); p[1] = (uint8_t)(v>>16); p[2] = (uint8_t)(v>>8); p[3] = (uint8_t)v; }
static int idx_name_cmp(const void *a, const void *b) { return strcmp(((const idx_ent *)a)->name, ((const idx_ent *)b)->name); }

static void idx_push(index_t *ix, const idx_ent *e) {
    if (ix->n == ix->cap) { ix->cap = ix->cap ? ix->cap * 2 : 64; ix->e = (idx_ent *)realloc(ix->e, ix->cap * sizeof *ix->e); }
    ix->e[ix->n++] = *e;
}
static void idx_upsert(index_t *ix, const idx_ent *e) {
    for (int i = 0; i < ix->n; i++) if (!strcmp(ix->e[i].name, e->name)) { ix->e[i] = *e; return; }
    idx_push(ix, e);
}
static int idx_load(index_t *ix) {
    ix->e = NULL; ix->n = ix->cap = 0;
    uint8_t *d; size_t n;
    if (read_whole(".git/index", &d, &n) != 0) return 0;
    if (n < 12 || memcmp(d, "DIRC", 4) || git_be32(d + 4) != 2) { free(d); return 0; }
    uint32_t cnt = git_be32(d + 8); size_t p = 12;
    for (uint32_t i = 0; i < cnt && p + 62 <= n; i++) {
        idx_ent e; memset(&e, 0, sizeof e);
        e.ctime_s = git_be32(d+p); e.mtime_s = git_be32(d+p+8);
        e.dev = git_be32(d+p+16); e.ino = git_be32(d+p+20); e.mode = git_be32(d+p+24);
        e.uid = git_be32(d+p+28); e.gid = git_be32(d+p+32); e.size = git_be32(d+p+36);
        memcpy(e.sha, d + p + 40, 20);
        size_t ns = p + 62, nl = 0;
        while (ns + nl < n && d[ns + nl] != 0 && nl < 255) nl++;
        memcpy(e.name, d + ns, nl); e.name[nl] = 0;
        size_t elen = (62 + nl + 1 + 7) & ~(size_t)7;
        p += elen;
        idx_push(ix, &e);
    }
    free(d);
    return 0;
}
static int idx_save(index_t *ix) {
    qsort(ix->e, ix->n, sizeof *ix->e, idx_name_cmp);
    buf b = {0};
    uint8_t h[12]; memcpy(h, "DIRC", 4); put32be(h + 4, 2); put32be(h + 8, (uint32_t)ix->n);
    buf_add(&b, h, 12);
    for (int i = 0; i < ix->n; i++) {
        idx_ent *e = &ix->e[i];
        uint8_t ent[62]; memset(ent, 0, 62);
        put32be(ent + 0, e->ctime_s); put32be(ent + 8, e->mtime_s);
        put32be(ent + 16, e->dev); put32be(ent + 20, e->ino); put32be(ent + 24, e->mode);
        put32be(ent + 28, e->uid); put32be(ent + 32, e->gid); put32be(ent + 36, e->size);
        memcpy(ent + 40, e->sha, 20);
        size_t nl = strlen(e->name); uint16_t fl = (uint16_t)(nl < 0xFFF ? nl : 0xFFF);
        ent[60] = (uint8_t)(fl >> 8); ent[61] = (uint8_t)fl;
        buf_add(&b, ent, 62);
        buf_add(&b, e->name, nl + 1);
        size_t pad = ((62 + nl + 1 + 7) & ~(size_t)7) - (62 + nl + 1);
        uint8_t zero[8] = {0}; if (pad) buf_add(&b, zero, pad);
    }
    uint8_t tr[20]; sha1(b.p, b.len, tr); buf_add(&b, tr, 20);
    int rc = write_whole(".git/index", b.p, b.len, 0644);
    buf_free(&b);
    return rc;
}

/* ---- write-tree: build tree objects from the (sorted) index ---- */
typedef struct { char name[256]; uint8_t sha[20]; unsigned mode; int is_dir; } tent;
static int tent_cmp(const void *pa, const void *pb) {
    const tent *a = (const tent *)pa, *b = (const tent *)pb;
    size_t la = strlen(a->name), lb = strlen(b->name), m = la < lb ? la : lb;
    int c = memcmp(a->name, b->name, m);
    if (c) return c;
    unsigned char ca = la > m ? (unsigned char)a->name[m] : (a->is_dir ? '/' : 0);
    unsigned char cb = lb > m ? (unsigned char)b->name[m] : (b->is_dir ? '/' : 0);
    return (int)ca - (int)cb;
}
static int tree_write(tent *ents, int n, uint8_t out[20]) {
    qsort(ents, n, sizeof *ents, tent_cmp);
    buf b = {0};
    for (int i = 0; i < n; i++) {
        char hdr[300]; int hn = snprintf(hdr, sizeof hdr, "%o %s", ents[i].mode, ents[i].name);
        buf_add(&b, hdr, hn); uint8_t z = 0; buf_add(&b, &z, 1); buf_add(&b, ents[i].sha, 20);
    }
    int rc = obj_write(GIT_TREE, b.p, b.len, out);
    buf_free(&b);
    return rc;
}
static void tent_push(tent **a, int *n, int *cap, const tent *t) {
    if (*n == *cap) { *cap = *cap ? *cap * 2 : 32; *a = (tent *)realloc(*a, *cap * sizeof **a); }
    (*a)[(*n)++] = *t;
}
static int build_tree(index_t *ix, int lo, int hi, size_t plen, uint8_t out[20]) {
    tent *ents = NULL; int nent = 0, cap = 0; int i = lo;
    while (i < hi) {
        const char *rel = ix->e[i].name + plen;
        const char *slash = strchr(rel, '/');
        tent te; memset(&te, 0, sizeof te);
        if (!slash) {
            strncpy(te.name, rel, 255); memcpy(te.sha, ix->e[i].sha, 20); te.mode = ix->e[i].mode; te.is_dir = 0;
            tent_push(&ents, &nent, &cap, &te);
            i++;
        } else {
            size_t dlen = (size_t)(slash - rel); int j = i;
            while (j < hi) { const char *r2 = ix->e[j].name + plen; if (!strncmp(r2, rel, dlen) && r2[dlen] == '/') j++; else break; }
            uint8_t sub[20];
            if (build_tree(ix, i, j, plen + dlen + 1, sub) != 0) { free(ents); return -1; }
            memcpy(te.name, rel, dlen); te.name[dlen] = 0; memcpy(te.sha, sub, 20); te.mode = 040000; te.is_dir = 1;
            tent_push(&ents, &nent, &cap, &te);
            i = j;
        }
    }
    int rc = tree_write(ents, nent, out);
    free(ents);
    return rc;
}
static int write_tree(index_t *ix, uint8_t out[20]) {
    qsort(ix->e, ix->n, sizeof *ix->e, idx_name_cmp);
    if (ix->n == 0) { die("git: nothing to write (empty index)"); return -1; }
    return build_tree(ix, 0, ix->n, 0, out);
}

/* ---- add ---- */
static void add_path(index_t *ix, const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) { die("git: pathspec did not match any files"); return; }
    if (S_ISDIR(st.st_mode)) {
        DIR *dp = opendir(path); if (!dp) return;
        struct dirent *de;
        while ((de = readdir(dp))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..") || !strcmp(de->d_name, ".git")) continue;
            char child[512]; snprintf(child, sizeof child, "%s/%s", path, de->d_name);
            add_path(ix, child);
        }
        closedir(dp);
    } else if (S_ISREG(st.st_mode)) {
        uint8_t *data; size_t len;
        if (read_whole(path, &data, &len) != 0) return;
        uint8_t sha[20];
        int rc = obj_write(GIT_BLOB, data, len, sha); free(data);
        if (rc != 0) return;
        idx_ent e; memset(&e, 0, sizeof e);
        e.mode = (st.st_mode & 0111) ? 0100755 : 0100644;
        e.size = (uint32_t)st.st_size; e.mtime_s = (uint32_t)st.st_mtime; e.ctime_s = (uint32_t)st.st_mtime;
        memcpy(e.sha, sha, 20);
        const char *nm = path; while (nm[0] == '.' && nm[1] == '/') nm += 2;
        strncpy(e.name, nm, 255);
        idx_upsert(ix, &e);
    }
}
static int cmd_add(int argc, char **argv) {
    if (!repo_ok()) return 1;
    index_t ix; idx_load(&ix);
    for (int i = 2; i < argc; i++) add_path(&ix, argv[i]);
    int rc = idx_save(&ix);
    free(ix.e);
    return rc;
}

/* ---- commit ---- */
static const char *commit_identity(void) { return "TinyOS <tinyos@localhost>"; }

static int cmd_commit(int argc, char **argv) {
    if (!repo_ok()) return 1;
    const char *msg = NULL;
    for (int i = 2; i < argc; i++) if (!strcmp(argv[i], "-m") && i + 1 < argc) { msg = argv[++i]; }
    if (!msg) { printf("usage: git commit -m <message>\n"); return 1; }

    index_t ix; idx_load(&ix);
    if (ix.n == 0) { die("git: nothing to commit"); free(ix.e); return 1; }
    uint8_t tree[20];
    if (write_tree(&ix, tree) != 0) { free(ix.e); return 1; }
    free(ix.e);

    uint8_t parent[20]; int hasp = (head_sha(parent) == 0);
    buf c = {0}; char L[600]; char hx[41];
    git_sha_hex(tree, hx);
    buf_add(&c, L, snprintf(L, sizeof L, "tree %s\n", hx));
    if (hasp) { git_sha_hex(parent, hx); buf_add(&c, L, snprintf(L, sizeof L, "parent %s\n", hx)); }
    long ts = (long)time(NULL); if (ts <= 0) ts = 1785500000;
    const char *who = commit_identity();
    buf_add(&c, L, snprintf(L, sizeof L, "author %s %ld +0000\ncommitter %s %ld +0000\n\n", who, ts, who, ts));
    buf_add(&c, msg, strlen(msg)); buf_add(&c, "\n", 1);

    uint8_t csha[20];
    int rc = obj_write(GIT_COMMIT, c.p, c.len, csha); buf_free(&c);
    if (rc != 0) { die("git: cannot write commit"); return 1; }

    char ref[160]; if (head_ref(ref, sizeof ref) != 0) strcpy(ref, "refs/heads/main");
    ref_write(ref, csha);
    char sh[41]; git_sha_hex(csha, sh);
    const char *br = strrchr(ref, '/'); br = br ? br + 1 : ref;
    printf("[%s %.*s%s] %s\n", br, 7, sh, hasp ? "" : " (root-commit)", msg);
    return 0;
}

/* ---- log ---- */
static int cmd_log(void) {
    if (!repo_ok()) return 1;
    uint8_t sha[20];
    if (head_sha(sha) != 0) { printf("no commits yet\n"); return 0; }
    for (int guard = 0; guard < 100000; guard++) {
        int type; uint8_t *d; size_t n;
        if (obj_read(sha, &type, &d, &n) != 0 || type != GIT_COMMIT) break;
        char h[41]; git_sha_hex(sha, h);
        printf("commit %s\n", h);
        /* parse header lines until the blank line, then the message */
        size_t i = 0; int have_parent = 0; uint8_t parent[20];
        while (i < n) {
            size_t e = i; while (e < n && d[e] != '\n') e++;
            if (e == i) { i = e + 1; break; }                 /* blank line -> message */
            if (e - i > 7 && !memcmp(d + i, "author ", 7)) printf("Author: %.*s\n", (int)(e - i - 7), d + i + 7);
            if (e - i >= 47 && !memcmp(d + i, "parent ", 7)) { if (git_hex_sha((char *)d + i + 7, parent) == 0) have_parent = 1; }
            i = e + 1;
        }
        printf("\n");
        while (i < n) { size_t e = i; while (e < n && d[e] != '\n') e++; printf("    %.*s\n", (int)(e - i), d + i); i = e + 1; }
        printf("\n");
        free(d);
        if (!have_parent) break;
        memcpy(sha, parent, 20);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* status: compare HEAD tree vs index vs working tree                 */
/* ------------------------------------------------------------------ */
typedef struct { char path[256]; uint8_t sha[20]; } pathent;
typedef struct { pathent *e; int n, cap; } pathlist;
static void pl_add(pathlist *pl, const char *p, const uint8_t *sha) {
    if (pl->n == pl->cap) { pl->cap = pl->cap ? pl->cap * 2 : 64; pl->e = (pathent *)realloc(pl->e, pl->cap * sizeof *pl->e); }
    strncpy(pl->e[pl->n].path, p, 255); pl->e[pl->n].path[255] = 0; memcpy(pl->e[pl->n].sha, sha, 20); pl->n++;
}
static pathent *pl_find(pathlist *pl, const char *p) {
    for (int i = 0; i < pl->n; i++) if (!strcmp(pl->e[i].path, p)) return &pl->e[i];
    return NULL;
}
static void tree_to_list(const uint8_t tsha[20], const char *prefix, pathlist *pl) {
    int type; uint8_t *d; size_t n;
    if (obj_read(tsha, &type, &d, &n) != 0 || type != GIT_TREE) return;
    gtree_iter it; git_tree_begin(&it, d, n);
    unsigned mode; const char *name; const uint8_t *esha;
    while (git_tree_next(&it, &mode, &name, &esha)) {
        char full[256]; snprintf(full, sizeof full, "%s%s", prefix, name);
        if (mode == 040000) { char np[520]; snprintf(np, sizeof np, "%s/", full); tree_to_list(esha, np, pl); }
        else pl_add(pl, full, esha);
    }
    free(d);
}
static void scan_worktree(const char *dir, const char *prefix, pathlist *pl) {
    DIR *dp = opendir(dir); if (!dp) return;
    struct dirent *de;
    while ((de = readdir(dp))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..") || !strcmp(de->d_name, ".git")) continue;
        char child[512]; snprintf(child, sizeof child, "%s/%s", dir, de->d_name);
        char rel[256]; snprintf(rel, sizeof rel, "%s%s", prefix, de->d_name);
        struct stat st; if (stat(child, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { char np[520]; snprintf(np, sizeof np, "%s/", rel); scan_worktree(child, np, pl); }
        else if (S_ISREG(st.st_mode)) {
            uint8_t *fd; size_t fn;
            if (read_whole(child, &fd, &fn) == 0) { uint8_t sha[20]; git_oid(GIT_BLOB, fd, fn, sha); free(fd); pl_add(pl, rel, sha); }
        }
    }
    closedir(dp);
}
static int cmd_status(void) {
    if (!repo_ok()) return 1;
    pathlist head = {0}, work = {0};
    uint8_t csha[20];
    if (head_sha(csha) == 0) {
        int t; uint8_t *d; size_t n;
        if (obj_read(csha, &t, &d, &n) == 0) { if (t == GIT_COMMIT) { uint8_t tsha[20]; if (git_commit_tree(d, n, tsha) == 0) tree_to_list(tsha, "", &head); } free(d); }
    }
    index_t ix; idx_load(&ix);
    scan_worktree(".", "", &work);

    char ref[160]; if (head_ref(ref, sizeof ref) != 0) strcpy(ref, "refs/heads/main");
    const char *br = strrchr(ref, '/'); br = br ? br + 1 : ref;
    printf("On branch %s\n", br);

    buf staged = {0}, unstaged = {0}, untracked = {0};
    char ln[320];
    for (int i = 0; i < ix.n; i++) {                       /* index vs HEAD -> staged */
        pathent *h = pl_find(&head, ix.e[i].name);
        if (!h) buf_add(&staged, ln, snprintf(ln, sizeof ln, "\tnew file:   %s\n", ix.e[i].name));
        else if (memcmp(h->sha, ix.e[i].sha, 20)) buf_add(&staged, ln, snprintf(ln, sizeof ln, "\tmodified:   %s\n", ix.e[i].name));
    }
    for (int i = 0; i < head.n; i++) {                     /* in HEAD but not index -> staged delete */
        int found = 0; for (int j = 0; j < ix.n; j++) if (!strcmp(ix.e[j].name, head.e[i].path)) { found = 1; break; }
        if (!found) buf_add(&staged, ln, snprintf(ln, sizeof ln, "\tdeleted:    %s\n", head.e[i].path));
    }
    for (int i = 0; i < ix.n; i++) {                       /* worktree vs index -> unstaged */
        pathent *w = pl_find(&work, ix.e[i].name);
        if (!w) buf_add(&unstaged, ln, snprintf(ln, sizeof ln, "\tdeleted:    %s\n", ix.e[i].name));
        else if (memcmp(w->sha, ix.e[i].sha, 20)) buf_add(&unstaged, ln, snprintf(ln, sizeof ln, "\tmodified:   %s\n", ix.e[i].name));
    }
    for (int i = 0; i < work.n; i++) {                     /* worktree not in index -> untracked */
        int found = 0; for (int j = 0; j < ix.n; j++) if (!strcmp(ix.e[j].name, work.e[i].path)) { found = 1; break; }
        if (!found) buf_add(&untracked, ln, snprintf(ln, sizeof ln, "\t%s\n", work.e[i].path));
    }

    if (staged.len) { printf("Changes to be committed:\n"); write(1, staged.p, staged.len); printf("\n"); }
    if (unstaged.len) { printf("Changes not staged for commit:\n"); write(1, unstaged.p, unstaged.len); printf("\n"); }
    if (untracked.len) { printf("Untracked files:\n"); write(1, untracked.p, untracked.len); printf("\n"); }
    if (!staged.len && !unstaged.len && !untracked.len) printf("nothing to commit, working tree clean\n");

    buf_free(&staged); buf_free(&unstaged); buf_free(&untracked);
    free(head.e); free(work.e); free(ix.e);
    return 0;
}

/* ------------------------------------------------------------------ */
/* drivers                                                            */
/* ------------------------------------------------------------------ */
static ref_t g_refs[4096];

/* Parse the packfile and write the HEAD commit's tree into dir. */
static int finish_clone(const char *want, buf *pack, const char *dir) {
    printf("received pack: %zu bytes\n", pack->len);
    gpack gp;
    if (git_parse_pack(pack->p, pack->len, &gp) != 0) { die("git: bad packfile"); return 1; }
    printf("unpacked %d objects\n", gp.n);
    uint8_t wsha[20], tsha[20];
    git_hex_sha(want, wsha);
    gobj *commit = git_find(&gp, wsha);
    if (!commit || commit->type != GIT_COMMIT) { die("git: HEAD commit not in pack"); git_free_pack(&gp); return 1; }
    if (git_commit_tree(commit->data, commit->len, tsha) != 0) { die("git: cannot read root tree"); git_free_pack(&gp); return 1; }
    mkdir(dir, 0755);
    if (checkout_tree(&gp, tsha, dir) != 0) { git_free_pack(&gp); return 1; }
    printf("checked out into %s/\n", dir);
    git_free_pack(&gp);
    return 0;
}

static int cmd_ls_remote(const char *urlstr) {
    url_t u;
    if (url_parse(urlstr, &u) != 0) return 1;
    char caps[512];
    int nc = u.ssh ? (ssh_start(&u) == 0 ? ssh_read_refs(g_refs, 4096, caps, sizeof caps) : -1)
                   : discover(&u, g_refs, 4096, caps, sizeof caps);
    if (nc < 0) return 1;
    for (int i = 0; i < nc; i++) printf("%s\t%s\n", g_refs[i].sha, g_refs[i].name);
    return 0;
}

static int cmd_clone(const char *urlstr, const char *dir, int depth) {
    url_t u;
    if (url_parse(urlstr, &u) != 0) return 1;

    char caps[512];
    printf("Cloning %s ...\n", urlstr);
    int nc = u.ssh ? (ssh_start(&u) == 0 ? ssh_read_refs(g_refs, 4096, caps, sizeof caps) : -1)
                   : discover(&u, g_refs, 4096, caps, sizeof caps);
    if (nc <= 0) { die("git: no refs discovered"); return 1; }

    /* choose the ref HEAD points at (fall back to the first ref) */
    const char *want = g_refs[0].sha;
    for (int i = 0; i < nc; i++)
        if (!strcmp(g_refs[i].name, "HEAD")) { want = g_refs[i].sha; break; }
    printf("want %s\n", want);

    buf pack = {0};
    int rc = u.ssh ? ssh_fetch(want, depth, &pack) : fetch_pack(&u, want, depth, &pack);
    if (rc != 0) { die("git: fetch failed"); buf_free(&pack); return 1; }
    rc = finish_clone(want, &pack, dir);
    buf_free(&pack);
    return rc;
}

static const char *basename_of(const char *path) {
    const char *b = path;
    for (const char *q = path; *q; q++) if (*q == '/') b = q + 1;
    return b;
}

int main(int argc, char **argv) {
    /* pull out a global "-i <key>" / "--key <key>" option anywhere on the line */
    static char *args[24]; int na = 0;
    for (int i = 0; i < argc && na < 24; i++) {
        if ((!strcmp(argv[i], "-i") || !strcmp(argv[i], "--key")) && i + 1 < argc) g_keypath = argv[++i];
        else args[na++] = argv[i];
    }
    argc = na; argv = args;

    if (argc >= 2 && !strcmp(argv[1], "init")) return cmd_init(argc > 2 ? argv[2] : "");
    if (argc >= 3 && !strcmp(argv[1], "hash-object")) return cmd_hash_object(argc, argv);
    if (argc >= 4 && !strcmp(argv[1], "cat-file")) return cmd_cat_file(argv[2], argv[3]);
    if (argc >= 3 && !strcmp(argv[1], "add")) return cmd_add(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "commit")) return cmd_commit(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "log")) return cmd_log();
    if (argc >= 2 && !strcmp(argv[1], "status")) return cmd_status();

    if (argc >= 3 && !strcmp(argv[1], "ls-remote"))
        return cmd_ls_remote(argv[2]);

    if (argc >= 3 && !strcmp(argv[1], "clone")) {
        int depth = 0, ai = 2;
        if (!strcmp(argv[ai], "--depth") && ai + 1 < argc) { depth = atoi(argv[ai + 1]); ai += 2; }
        if (ai >= argc) { printf("usage: git clone [--depth N] [-i key] <url> [dir]\n"); return 1; }
        const char *url = argv[ai++];
        char dbuf[256];
        const char *dir;
        if (ai < argc) dir = argv[ai];
        else {
            const char *b = basename_of(url);
            size_t bl = strlen(b);
            if (bl > 4 && !strcmp(b + bl - 4, ".git")) bl -= 4;
            if (bl >= sizeof dbuf) bl = sizeof dbuf - 1;
            memcpy(dbuf, b, bl); dbuf[bl] = 0;
            dir = dbuf[0] ? dbuf : "repo";
        }
        return cmd_clone(url, dir, depth);
    }

    printf("usage:\n"
           "  git init [dir]\n"
           "  git hash-object [-w] <file>\n"
           "  git cat-file (-t|-s|-p) <sha>\n"
           "  git add <path>...\n"
           "  git commit -m <message>\n"
           "  git log\n"
           "  git status\n"
           "  git ls-remote [-i key] <url>\n"
           "  git clone [--depth N] [-i key] <url> [dir]\n"
           "  urls: https://host/owner/repo | git@host:owner/repo | ssh://git@host/owner/repo\n");
    return 1;
}
