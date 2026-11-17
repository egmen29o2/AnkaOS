/* ===========================================================================
 * AnkaOS - çekirdek ana başlığı: tüm alt sistemlerin arayüzleri
 * =========================================================================== */
#ifndef KERNEL_H
#define KERNEL_H

#include <types.h>
#include <lib.h>
#include <abi.h>

#define KERNEL_NAME     "AnkaOS"
#define KERNEL_VERSION  "0.1.0"
#define TIMER_HZ        100
#define MS_PER_TICK     (1000 / TIMER_HZ)
#define PAGE_SIZE       4096u
#define MAX_TASKS       32

/* ---- port G/Ç ve CPU yardımcıları ---------------------------------------- */
static inline void outb(u16 p, u8 v)  { __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(p)); }
static inline void outw(u16 p, u16 v) { __asm__ volatile("outw %0, %1" : : "a"(v), "Nd"(p)); }
static inline u8  inb(u16 p) { u8 v;  __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline u16 inw(u16 p) { u16 v; __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline void io_wait(void) { outb(0x80, 0); }

/* kesmeleri kapat, önceki EFLAGS'i döndür / geri yükle */
static inline u32 irq_save(void)
{
    u32 f;
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(f) : : "memory");
    return f;
}
static inline void irq_restore(u32 f)
{
    if (f & 0x200) __asm__ volatile("sti" : : : "memory");
}

static inline u32  read_cr0(void) { u32 v; __asm__ volatile("mov %%cr0, %0" : "=r"(v)); return v; }
static inline u32  read_cr2(void) { u32 v; __asm__ volatile("mov %%cr2, %0" : "=r"(v)); return v; }
static inline u32  read_cr3(void) { u32 v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return v; }
static inline void write_cr0(u32 v) { __asm__ volatile("mov %0, %%cr0" : : "r"(v) : "memory"); }
static inline void write_cr3(u32 v) { __asm__ volatile("mov %0, %%cr3" : : "r"(v) : "memory"); }
static inline void invlpg(u32 a) { __asm__ volatile("invlpg (%0)" : : "r"(a) : "memory"); }

/* ---- kesme çerçevesi (entry.S isr_common ile birebir aynı sıra) ------------ */
struct regs {
    u32 gs, fs, es, ds;
    u32 edi, esi, ebp, esp, ebx, edx, ecx, eax;   /* pusha */
    u32 int_no, err_code;
    u32 eip, cs, eflags;                          /* CPU'nun ittiği kısım */
    u32 useresp, ss;                              /* yalnızca ring 3'ten gelince */
};

/* ---- önyükleyici bilgisi (boot.S yazar) ------------------------------------ */
struct e820_entry { u64 base; u64 length; u32 type; u32 acpi; } PACKED;
struct bootinfo {
    u32 magic;            /* "ANKA" = 0x414B4E41 */
    u32 drive;
    u32 e820_count;
    u32 reserved;
    struct e820_entry e820[40];
} PACKED;
#define BOOTINFO_ADDR  0x500
#define BOOTINFO_MAGIC 0x414B4E41u

/* ---- ekran / seri / konsol -------------------------------------------------- */
#define KEY_UP    0x101
#define KEY_DOWN  0x102
#define KEY_LEFT  0x103
#define KEY_RIGHT 0x104
#define KEY_HOME  0x105
#define KEY_END   0x106
#define KEY_DEL   0x107

void vga_init(void);
void vga_clear(void);
void vga_putc(char c);
void vga_sync_cursor(void);
void vga_set_color(u8 fg, u8 bg);
u8   vga_get_attr(void);
void vga_set_attr(u8 attr);
void vga_recolor(u8 attr);

void serial_init(void);
void serial_putc(char c);
void serial_irq_init(void);

void console_putc(char c);
void console_write(const char *s, int n);
int  kprintf(const char *fmt, ...);
void kputs(const char *s);
void input_push(u16 key);
u16  input_get(void);
void input_flush(void);
int  console_readline(char *buf, int max, int use_hist);   /* >=0 uzunluk, -1 Ctrl-C, -2 EOF */
void console_raise_sigint(void);
int  console_take_sigint(void);
void backtrace(u32 ebp);
NORETURN void kpanic(const char *fmt, ...);

/* ---- mimari: GDT/TSS, IDT, PIC, PIT, klavye -------------------------------- */
void gdt_init(void);
void tss_set_kstack(u32 esp0);
void idt_init(void);
typedef void (*irq_handler_t)(struct regs *);
void irq_install(int irq, irq_handler_t h);
void irq_unmask(int irq);
void pic_init(void);
void pic_eoi(int irq);
int  pic_spurious(int irq);
void isr_handler(struct regs *r);          /* entry.S çağırır */
extern volatile u32 ticks;
void pit_init(u32 hz);
u32  uptime_ms(void);
void kbd_init(void);

/* ---- bellek yönetimi --------------------------------------------------------- */
#define PTE_P  0x001u
#define PTE_RW 0x002u
#define PTE_US 0x004u

void pmm_init(const struct bootinfo *bi);
u32  pmm_alloc(void);                      /* 0 = bellek yok, aksi halde fiziksel adres */
u32  pmm_alloc_zeroed(void);
u32  pmm_alloc_contig(u32 count);
void pmm_free(u32 phys);
u32  pmm_total_frames(void);
u32  pmm_free_frames(void);
u32  pmm_mem_top(void);

void paging_init(void);
u32  vmm_kernel_pd(void);
u32  vmm_current(void);
void vmm_switch(u32 pd);
u32  vmm_create_space(void);
void vmm_destroy_space(u32 pd);
int  vmm_map(u32 pd, u32 vaddr, u32 paddr, u32 flags);
int  vmm_user_ok(u32 pd, u32 addr, u32 len, int write);

void  heap_init(u32 start, u32 size);
void *kmalloc(u32 size);
void  kfree(void *p);
void *krealloc(void *p, u32 size);
void  heap_stats(u32 *used, u32 *free_bytes, u32 *blocks);

/* ---- görevler / işlemler --------------------------------------------------- */
typedef enum { T_UNUSED = 0, T_RUNNABLE, T_RUNNING, T_SLEEPING, T_BLOCKED, T_ZOMBIE } tstate_t;
#define WAIT_INPUT 1

typedef struct task {
    u32 esp;                      /* kaydedilmiş çekirdek yığın göstergesi */
    int pid;
    tstate_t state;
    char name[16];
    u32 kstack, kstack_top;
    u32 pd;                       /* sayfa dizini (fiziksel); 0 = çekirdek görevi */
    u32 wake_tick;
    int wait_reason;
    int exit_code;
    int slice;
    u32 cpu_ticks;
    int detached, is_user, protect;
    void (*entry)(void *);
    void *arg;
    u32 user_entry, user_esp;
    u32 start_tick;
} task_t;

extern task_t *current;
extern volatile int need_resched;

void    task_init(void);
task_t *task_create(const char *name, void (*entry)(void *), void *arg, u32 pd, int detached);
void    schedule(void);
void    task_yield(void);
void    task_sleep(u32 ms);
void    task_block(int reason);
void    task_wake(int reason);
void    task_tick(void);
NORETURN void task_exit(int code);
int     task_kill(int pid);
int     task_poll(int pid, int *code);     /* 1 = bitti (ve toplandı), 0 = sürüyor, -1 = yok */
task_t *task_at(int idx);
int     process_exec(const char *name, const u8 *img, u32 size, int argc, char **argv, int detached);

void syscall_handler(struct regs *r);

/* gömülü kullanıcı programları (progs.S) */
struct prog { const char *name; const u8 *start; const u8 *end; };
extern const struct prog progs[];
extern const int nprogs;

/* ---- RAM dosya sistemi ------------------------------------------------------- */
#define FS_NAME_MAX 31
#define FS_FILE 1
#define FS_DIR  2
typedef struct fsnode {
    char name[FS_NAME_MAX + 1];
    u8 type;
    struct fsnode *parent, *child, *next;
    u8 *data;
    u32 size, cap;
} fsnode_t;
enum { FS_OK = 0, FS_EBUSY = -16, FS_ENOENT = -2, FS_ENOMEM = -12, FS_EEXIST = -17,
       FS_ENOTDIR = -20, FS_EISDIR = -21, FS_EINVAL = -22, FS_ENAMETOOLONG = -36,
       FS_ENOTEMPTY = -39 };

void       fs_init(void);
fsnode_t  *fs_root(void);
fsnode_t  *fs_resolve(fsnode_t *cwd, const char *path);
int        fs_split(fsnode_t *cwd, const char *path, fsnode_t **dir, char *name);
int        fs_create(fsnode_t *cwd, const char *path, int type, fsnode_t **out);
int        fs_read(fsnode_t *f, u32 off, void *buf, u32 len);
int        fs_write(fsnode_t *f, u32 off, const void *buf, u32 len);
void       fs_truncate(fsnode_t *f);
int        fs_remove(fsnode_t *n);
int        fs_move(fsnode_t *n, fsnode_t *new_parent, const char *new_name);
void       fs_path(fsnode_t *n, char *buf, int size);
const char *fs_strerror(int err);

void shell_main(void *arg);

extern char boot_stack_top[];
extern char __kernel_start[], __kernel_end[];

#endif
