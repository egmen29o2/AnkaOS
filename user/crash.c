/* crash [kernel|null|cli|div|hlt|badptr|stack] - bellek/ayrıcalık korumasını gösterir:
 * işlem öldürülür (veya sistem çağrısı hata döner), çekirdek yaşar */
#include "ulib.h"

static int __attribute__((noinline)) recurse(int n)
{
    volatile char pad[512];
    pad[0] = (char)n;
    return recurse(n + 1) + pad[0];
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "kernel";
    printf("crash: mode '%s'\n", mode);
    if (!strcmp(mode, "kernel")) {
        printf("writing to kernel memory at 0x00100000...\n");
        *(volatile int *)0x00100000 = 42;
    } else if (!strcmp(mode, "null")) {
        printf("reading address 0...\n");
        return *(volatile int *)0;
    } else if (!strcmp(mode, "cli")) {
        printf("executing privileged instruction (cli)...\n");
        __asm__ volatile("cli");
    } else if (!strcmp(mode, "div")) {
        volatile int num = 100, zero = 0;          /* 1/x gibi sabit bölünenler derleyici tarafından kaldırılır */
        printf("100/0 = %d\n", num / zero);
    } else if (!strcmp(mode, "hlt")) {
        __asm__ volatile("hlt");
    } else if (!strcmp(mode, "badptr")) {
        printf("write(kernel ptr) = %d (expected -1)\n", write(1, (const void *)0x00100000, 16));
        printf("write(end of user space) = %d (expected -1)\n", write(1, (const void *)(USER_LIMIT - 4), 64));
        printf("read(unmapped ptr) = %d (expected -1)\n", read(0, (void *)0x1000, 8));
        return 0;
    } else if (!strcmp(mode, "stack")) {
        printf("recursing until the stack overflows...\n");
        return recurse(1);
    } else {
        printf("unknown mode. try: kernel null cli div hlt badptr stack\n");
        return 2;
    }
    printf("UNREACHABLE: protection did not work!\n");
    return 0;
}
