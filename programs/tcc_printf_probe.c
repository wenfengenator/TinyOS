/* SPDX-License-Identifier: GPL-2.0-only */

#include <stdio.h>
#include <unistd.h>

int main(void) {
    char *const argv[] = {
        (char *)"/tcc.bin",
        (char *)"@/printf.rsp",
        NULL
    };
    char *const envp[] = { (char *)"PATH=/", NULL };

    printf("[tcc] compiling libc printf app -> /printf.bin\n");
    execve(argv[0], argv, envp);
    perror("execve tcc printf");
    return 1;
}