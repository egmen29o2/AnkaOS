/* AnkaOS - kullanıcı modu kütüphanesi */
#include "ulib.h"

NORETURN void exit(int code)
{
    sys3(SYS_EXIT, code, 0, 0);
    for (;;) { }
}

int  write(int fd, const void *buf, int len) { return sys3(SYS_WRITE, fd, (int)buf, len); }
int  read(int fd, void *buf, int len)        { return sys3(SYS_READ, fd, (int)buf, len); }
int  getpid(void)                            { return sys3(SYS_GETPID, 0, 0, 0); }
void sleep_ms(u32 ms)                        { sys3(SYS_SLEEP, (int)ms, 0, 0); }
void yield(void)                             { sys3(SYS_YIELD, 0, 0, 0); }
u32  uptime_ms(void)                         { return (u32)sys3(SYS_UPTIME, 0, 0, 0); }

int printf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    write(1, buf, (int)strlen(buf));
    return n;
}
