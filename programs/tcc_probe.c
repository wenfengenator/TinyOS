/* SPDX-License-Identifier: GPL-2.0-only */

#include <stdio.h>
#include <unistd.h>

int main(void) {
    char *const argv[] = {
        (char *)"/tcc.bin",
        (char *)"-nostdlib",
        (char *)"-static",
        (char *)"-Wl,-e,_start",
        (char *)"-Wl,-Ttext=0x400000",
        (char *)"-o",
        (char *)"/made.bin",
        (char *)"/hello.c",
        NULL
    };
    char *const envp[] = { (char *)"PATH=/", NULL };

    printf("[tcc] compiling /hello.c -> /made.bin\n");
    execve(argv[0], argv, envp);
    perror("execve tcc");
    return 1;
}
