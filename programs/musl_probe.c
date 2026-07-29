#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    static const char path[] = "/musl.txt";
    static const char expected[] = "musl probe\n";
    size_t len = strlen(expected);
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    if (write(fd, expected, len) != (ssize_t)len || lseek(fd, 0, SEEK_SET) != 0) {
        perror("write/lseek");
        close(fd);
        return 1;
    }

    char *buf = malloc(len + 1);
    if (!buf) {
        close(fd);
        return 1;
    }
    ssize_t count = read(fd, buf, len);
    if (count != (ssize_t)len) {
        perror("read");
        free(buf);
        close(fd);
        return 1;
    }
    buf[count] = 0;

    if (strcmp(buf, expected) != 0) {
        fprintf(stderr, "content mismatch\n");
        free(buf);
        close(fd);
        return 1;
    }

    printf("[musl] printf + malloc + file I/O verified: %s", buf);
    free(buf);
    if (close(fd) != 0) {
        perror("close");
        return 1;
    }
    return 0;
}
