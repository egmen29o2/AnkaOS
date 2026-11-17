/* AnkaOS - kullanıcı programları için mini kütüphane (sistem çağrı sarmalayıcıları) */
#ifndef ULIB_H
#define ULIB_H

#include <types.h>
#include <lib.h>
#include <abi.h>

static inline int sys3(int n, int a, int b, int c)
{
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}

NORETURN void exit(int code);
int  write(int fd, const void *buf, int len);
int  read(int fd, void *buf, int len);
int  getpid(void);
void sleep_ms(u32 ms);
void yield(void);
u32  uptime_ms(void);
int  printf(const char *fmt, ...);

#endif
