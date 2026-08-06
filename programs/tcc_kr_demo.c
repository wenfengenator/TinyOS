/* SPDX-License-Identifier: GPL-2.0-only */

/* A small K&R-style showcase: exercises <stdio.h>, <ctype.h>, <stdlib.h>, and
 * <string.h> from the on-device musl headers.  Self-contained (no stdin) so it
 * can be built and run non-interactively. */
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define IN  1
#define OUT 0

int main(void) {
    const char *text = "the quick brown fox\njumps over the lazy dog\n";
    int c, lines = 0, words = 0, chars = 0, state = OUT;

    for (const char *p = text; (c = *p ? *p++ : EOF) != EOF; ) {
        chars++;
        if (c == '\n') lines++;
        if (c == ' ' || c == '\n' || c == '\t') state = OUT;
        else if (state == OUT) { state = IN; words++; }
    }

    char upper[64];
    strcpy(upper, "K&R");
    printf("%s word count -> lines=%d words=%d chars=%d\n", upper, lines, words, chars);
    printf("atoi(\"1978\") + 1 = %d, toupper('c') = %c\n", atoi("1978") + 1, toupper('c'));
    return 0;
}
