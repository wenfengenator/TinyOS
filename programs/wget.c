/* SPDX-License-Identifier: GPL-2.0-only */

/* wget — a minimal HTTP/1.0 client for TinyOS (Cycle 8 N4).
 *
 * Resolves a host (DNS via the kernel resolver, or a dotted-quad literal),
 * opens a TCP connection with the BSD socket API, sends a GET, and writes the
 * response to stdout.  Build on device:  ./tcc.bin /wget.c -o /wget.bin
 *
 *   wget <host> [path]      e.g.  wget example.com /   |   wget 10.0.2.2 / */
#include <tnet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: wget <host> [path]\n"); return 1; }
    const char *host = argv[1];
    const char *path = argc > 2 ? argv[2] : "/";

    uint32_t ip;
    uint32_t lit = ntohl(inet_addr(host));               /* dotted-quad literal? */
    if (lit != 0xFFFFFFFFu && lit != 0) ip = lit;
    else ip = net_resolve(host);
    if (!ip) { printf("wget: cannot resolve %s\n", host); return 1; }

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { printf("wget: socket failed\n"); return 1; }
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(80);
    a.sin_addr.s_addr = htonl(ip);
    if (connect(s, (struct sockaddr *)&a, sizeof a) != 0) {
        printf("wget: connect to %u.%u.%u.%u failed\n",
               (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
        close(s); return 1;
    }

    char req[512];
    int rl = snprintf(req, sizeof req,
                      "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", path, host);
    write(s, req, rl);

    char buf[2048]; int n;
    while ((n = read(s, buf, sizeof buf)) > 0) write(1, buf, n);
    close(s);
    return 0;
}
