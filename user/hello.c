/* hello - ring 3'te çalışan en basit program */
#include "ulib.h"

int main(int argc, char **argv)
{
    printf("Hello from user mode! (pid %d, ring 3)\n", getpid());
    for (int i = 1; i < argc; i++) printf("  arg[%d] = %s\n", i, argv[i]);
    for (int i = 1; i <= 3; i++) {
        printf("  tick %d  (uptime %u ms)\n", i, uptime_ms());
        sleep_ms(500);
    }
    printf("Bye!\n");
    return 0;
}
