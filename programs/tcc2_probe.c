#include <stdio.h>
#include <unistd.h>

int main(void) {
    char *const argv[] = {
        (char *)"/tcc2.bin",
        (char *)"-nostdlib",
        (char *)"-static",
        (char *)"-Wl,-e,_start",
        (char *)"-Wl,-Ttext=0x400000",
        (char *)"-o",
        (char *)"/made2.bin",
        (char *)"/hello.c",
        NULL
    };
    char *const envp[] = { (char *)"PATH=/", NULL };

    printf("[tcc-self] rebuilt TCC compiling /hello.c\n");
    execve(argv[0], argv, envp);
    perror("execve rebuilt tcc");
    return 1;
}
