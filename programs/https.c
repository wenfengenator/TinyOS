/* https — a minimal HTTPS/1.0 client for TinyOS (Cycle 9 C5).
 *
 * Resolves a host, opens a TCP connection, runs the TLS 1.3 client handshake
 * (ttls.h), sends a GET over the encrypted channel, and writes the response to
 * stdout.  Build on device:  ./tcc.bin /https.c -o /https.bin
 *
 *   https <url>            e.g.  https https://example.com/   |   https example.com /
 *
 * SECURITY: the server certificate is NOT verified yet (see ttls.h) — the
 * connection is encrypted but not authenticated.  A warning is printed. */
#include <tnet.h>
#include <ttls.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: https <url>   e.g. https https://example.com/\n"); return 1; }

    char host[256]; int port = 443;
    static char pathbuf[1024]; const char *path = "/";
    const char *u = argv[1];
    if (!strncmp(u, "https://", 8)) u += 8;
    else if (!strncmp(u, "http://", 7)) { printf("https: not an https URL (use wget for http)\n"); return 1; }
    int hi = 0;
    while (*u && *u != '/' && *u != ':' && hi < 255) host[hi++] = *u++;
    host[hi] = 0;
    if (*u == ':') { port = atoi(u + 1); while (*u && *u != '/') u++; }
    if (*u == '/') { strncpy(pathbuf, u, sizeof pathbuf - 1); pathbuf[sizeof pathbuf - 1] = 0; path = pathbuf; }
    else if (argc > 2) path = argv[2];
    if (!host[0]) { printf("https: empty host\n"); return 1; }

    uint32_t lit = ntohl(inet_addr(host));                   /* dotted-quad literal? */
    uint32_t ip = (lit != 0xFFFFFFFFu && lit != 0) ? lit : net_resolve(host);
    if (!ip) { printf("https: cannot resolve %s\n", host); return 1; }

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { printf("https: socket failed\n"); return 1; }
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((unsigned short)port); a.sin_addr.s_addr = htonl(ip);
    if (connect(s, (struct sockaddr *)&a, sizeof a) != 0) {
        printf("https: connect to %u.%u.%u.%u:%d failed\n",
               (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF, port);
        close(s); return 1;
    }

    static tls_conn c;                                       /* ~17KB (record buffer) -> keep off the small user stack */
    int r = tls_client_handshake(&c, s, host);
    if (r != 0) { printf("https: TLS handshake failed (rc=%d)\n", r); close(s); return 1; }
    if (!tls_is_authenticated(&c)) {
        const char *w = "https: WARNING: server certificate not verified (encrypted, not authenticated)\n";
        write(2, w, strlen(w));
    }

    char req[1200];
    int rl = snprintf(req, sizeof req,
                      "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: tinyos-https/1.0\r\nConnection: close\r\n\r\n",
                      path, host);
    if (rl > 0 && rl < (int)sizeof req) tls_write(&c, req, (size_t)rl);

    static char buf[4096]; int n;
    while ((n = tls_read(&c, buf, sizeof buf)) > 0) write(1, buf, n);
    tls_close(&c);
    return 0;
}
