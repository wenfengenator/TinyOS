/* SPDX-License-Identifier: GPL-2.0-only */

/* Heap demo: malloc / calloc / realloc / free against the on-device musl libc.
 * Writes and reads back real memory, and stress-cycles the allocator. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    int *a = malloc(1000 * sizeof(int));
    if (!a) { printf("malloc failed\n"); return 1; }
    for (int i = 0; i < 1000; i++) a[i] = i * i;
    long sum = 0;
    for (int i = 0; i < 1000; i++) sum += a[i];

    a = realloc(a, 2000 * sizeof(int));
    if (!a) { printf("realloc failed\n"); return 1; }
    for (int i = 1000; i < 2000; i++) a[i] = i;

    char *s = calloc(32, 1);
    strcpy(s, "heap ok");
    printf("sum(i*i,0..999)=%ld realloc[1999]=%d %s\n", sum, a[1999], s);
    free(s);
    free(a);

    for (int k = 0; k < 4000; k++) {
        void *p = malloc(4096);
        if (!p) { printf("cycle %d failed\n", k); return 1; }
        free(p);
    }
    printf("malloc/free: 4000 cycles ok\n");
    return 0;
}
