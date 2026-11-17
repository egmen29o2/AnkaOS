/* AnkaOS - GDT ve TSS
 *   0x00 boş | 0x08 çekirdek kodu | 0x10 çekirdek verisi
 *   0x18 kullanıcı kodu (DPL3) | 0x20 kullanıcı verisi (DPL3) | 0x28 TSS */
#include <kernel.h>

struct gdt_entry {
    u16 limit_low, base_low;
    u8 base_mid, access, gran, base_high;
} PACKED;

struct gdtr { u16 limit; u32 base; } PACKED;

struct tss {
    u32 prev_tss, esp0, ss0, esp1, ss1, esp2, ss2, cr3, eip, eflags;
    u32 eax, ecx, edx, ebx, esp, ebp, esi, edi;
    u32 es, cs, ss, ds, fs, gs, ldt;
    u16 trap, iomap_base;
} PACKED;

static struct gdt_entry gdt[6];
static struct gdtr gdt_ptr;
static struct tss tss;

extern void gdt_flush(const void *gdtr);
extern void tss_flush(void);

static void gdt_set(int i, u32 base, u32 limit, u8 access, u8 gran)
{
    gdt[i].limit_low = (u16)(limit & 0xFFFF);
    gdt[i].base_low  = (u16)(base & 0xFFFF);
    gdt[i].base_mid  = (u8)((base >> 16) & 0xFF);
    gdt[i].access    = access;
    gdt[i].gran      = (u8)(((limit >> 16) & 0x0F) | (gran & 0xF0));
    gdt[i].base_high = (u8)((base >> 24) & 0xFF);
}

void gdt_init(void)
{
    memset(&tss, 0, sizeof tss);
    tss.ss0 = 0x10;
    tss.esp0 = (u32)boot_stack_top;
    tss.iomap_base = sizeof tss;        /* G/Ç bitmap'i yok: ring 3'te tüm portlar yasak */

    gdt_set(0, 0, 0, 0, 0);
    gdt_set(1, 0, 0xFFFFF, 0x9A, 0xCF);
    gdt_set(2, 0, 0xFFFFF, 0x92, 0xCF);
    gdt_set(3, 0, 0xFFFFF, 0xFA, 0xCF);
    gdt_set(4, 0, 0xFFFFF, 0xF2, 0xCF);
    gdt_set(5, (u32)&tss, sizeof tss - 1, 0x89, 0x00);

    gdt_ptr.limit = sizeof gdt - 1;
    gdt_ptr.base = (u32)gdt;
    gdt_flush(&gdt_ptr);
    tss_flush();
}

/* ring 3 -> ring 0 geçişinde CPU'nun kullanacağı çekirdek yığını */
void tss_set_kstack(u32 esp0) { tss.esp0 = esp0; }
