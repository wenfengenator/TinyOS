static const char message[] = "[tcc] on-device compiled ELF executed\n";

void _start(void) {
    __asm__ volatile(
        "mov $1, %%rax\n"
        "mov $1, %%rdi\n"
        "mov %0, %%rsi\n"
        "mov %1, %%rdx\n"
        "syscall\n"
        "mov $231, %%rax\n"
        "xor %%rdi, %%rdi\n"
        "syscall\n"
        :
        : "r"(message), "r"(sizeof(message) - 1)
        : "rax", "rdi", "rsi", "rdx", "rcx", "r11", "memory");
    __builtin_unreachable();
}
