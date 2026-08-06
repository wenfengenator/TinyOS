/* SPDX-License-Identifier: GPL-2.0-only */

/* httpd — a tiny HTTP/1.0 server for TinyOS (Cycle 8 N4).
 *
 * Listens on a TCP port and serves one small page per connection using the BSD
 * socket API.  From the host it is reachable through the QEMU SLIRP host-forward
 * (see the Makefile run targets: host localhost:8080 -> guest :80).
 *
 *   httpd [port]            default port 80.   Build:  ./tcc.bin /httpd.c -o /httpd.bin */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

int main(int argc, char **argv) {
    int port = argc > 1 ? atoi(argv[1]) : 80;

    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) { printf("httpd: socket failed\n"); return 1; }
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    a.sin_addr.s_addr = htonl(0);                        /* INADDR_ANY */
    if (bind(ls, (struct sockaddr *)&a, sizeof a) != 0) { printf("httpd: bind failed\n"); return 1; }
    if (listen(ls, 4) != 0) { printf("httpd: listen failed\n"); return 1; }
    printf("httpd: listening on port %d (Ctrl-C to stop)\n", port);

    const char *body = "<html><body><h1>Hello from TinyOS</h1>"
                       "<p>Served by a hand-written TCP stack.</p></body></html>\n";
    for (;;) {
        int cs = accept(ls, 0, 0);
        if (cs < 0) break;
        char req[1024];
        int n = read(cs, req, sizeof req - 1);
        if (n > 0) req[n] = 0;
        char resp[512];
        int rl = snprintf(resp, sizeof resp,
                          "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n"
                          "Content-Length: %d\r\nConnection: close\r\n\r\n%s",
                          (int)strlen(body), body);
        write(cs, resp, rl);
        close(cs);
    }
    close(ls);
    return 0;
}
