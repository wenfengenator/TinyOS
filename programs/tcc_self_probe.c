#include <stdio.h>
#include <unistd.h>

int main(void) {
    char *const argv[] = {
        (char *)"/tcc.bin",
        (char *)"@/build.rsp",
        NULL
    };
    char *const envp[] = { (char *)"PATH=/", NULL };

    printf("[tcc-self] rebuilding TCC -> /tcc2.bin\n");
    execve(argv[0], argv, envp);
    perror("execve tcc self-host");
    return 1;
}
