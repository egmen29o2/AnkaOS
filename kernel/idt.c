/* AnkaOS - IDT: 0-31 istisna, 32-47 IRQ, 0x80 sistem çağrısı (ring 3 erişebilir) */
#include <kernel.h>

struct idt_entry { u16 off_lo, sel; u8 zero, flags; u16 off_hi; } PACKED;
struct idtr { u16 limit; u32 base; } PACKED;

static struct idt_entry idt[256];
static struct idtr idt_ptr;

extern const u32 isr_stub_table[48];
extern void isr128(void);
extern void idt_flush(const void *idtr);

static void idt_set(int n, u32 handler, u8 flags)
{
    idt[n].off_lo = (u16)(handler & 0xFFFF);
    idt[n].sel    = 0x08;
    idt[n].zero   = 0;
    idt[n].flags  = flags;
    idt[n].off_hi = (u16)(handler >> 16);
}

void idt_init(void)
{
    memset(idt, 0, sizeof idt);
    for (int i = 0; i < 48; i++) idt_set(i, isr_stub_table[i], 0x8E);   /* P, DPL0, kesme kapısı */
    idt_set(0x80, (u32)isr128, 0xEF);                                   /* P, DPL3, tuzak kapısı */
    idt_ptr.limit = sizeof idt - 1;
    idt_ptr.base = (u32)idt;
    idt_flush(&idt_ptr);
}
