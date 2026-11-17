/* AnkaOS - çekirdek <-> kullanıcı modu sözleşmesi (paylaşılan başlık) */
#ifndef ABI_H
#define ABI_H

/* Kullanıcı sanal adres düzeni (her işlemin kendi sayfa dizini vardır) */
#define USER_BASE        0x40000000u   /* program imajı buraya yüklenir      */
#define USER_LIMIT       0x80000000u   /* kullanıcı adres alanının sonu      */
#define USER_STACK_TOP   USER_LIMIT    /* yığın aşağı doğru büyür            */
#define USER_STACK_PAGES 4

/* Düz (flat) çalıştırılabilir dosya başlığı - imajın en başında durur */
#define EXEC_MAGIC 0x414B5845u         /* 'E','X','K','A' */
struct exec_header {
    unsigned int magic;
    unsigned int memsz;                /* bss dahil toplam bellek boyutu */
    unsigned int entry;                /* giriş noktasının sanal adresi  */
};

/* Sistem çağrıları: int 0x80, eax=numara, ebx/ecx/edx=argümanlar, dönüş=eax */
enum {
    SYS_EXIT   = 1,   /* exit(code)                  */
    SYS_WRITE  = 2,   /* write(fd, buf, len)         */
    SYS_READ   = 3,   /* read(fd, buf, len)  (satır) */
    SYS_GETPID = 4,   /* getpid()                    */
    SYS_SLEEP  = 5,   /* sleep(ms)                   */
    SYS_YIELD  = 6,   /* yield()                     */
    SYS_UPTIME = 7,   /* uptime() -> ms              */
};

#endif
