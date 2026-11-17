/* AnkaOS - 8259 PIC: IRQ'ları 0x20..0x2F vektörlerine taşır */
#include <kernel.h>

#define PIC1 0x20
#define PIC2 0xA0

void pic_init(void)
{
    outb(PIC1, 0x11);     io_wait();      /* ICW1: başlat, ICW4 gerekli */
    outb(PIC2, 0x11);     io_wait();
    outb(PIC1 + 1, 0x20); io_wait();      /* ICW2: vektör ofsetleri */
    outb(PIC2 + 1, 0x28); io_wait();
    outb(PIC1 + 1, 0x04); io_wait();      /* ICW3: slave IRQ2'de */
    outb(PIC2 + 1, 0x02); io_wait();
    outb(PIC1 + 1, 0x01); io_wait();      /* ICW4: 8086 modu */
    outb(PIC2 + 1, 0x01); io_wait();
    outb(PIC1 + 1, 0xFF);                 /* hepsi maskeli; sürücüler açar */
    outb(PIC2 + 1, 0xFF);
}

void irq_unmask(int irq)
{
    u16 port = irq < 8 ? PIC1 + 1 : PIC2 + 1;
    outb(port, (u8)(inb(port) & ~(1u << (irq & 7))));
    if (irq >= 8) outb(PIC1 + 1, (u8)(inb(PIC1 + 1) & ~0x04));   /* cascade */
}

void pic_eoi(int irq)
{
    if (irq >= 8) outb(PIC2, 0x20);
    outb(PIC1, 0x20);
}

/* IRQ7/IRQ15 sahte (spurious) kesme olabilir: ISR yazmacına bak */
int pic_spurious(int irq)
{
    if (irq == 7) {
        outb(PIC1, 0x0B);
        return !(inb(PIC1) & 0x80);
    }
    if (irq == 15) {
        outb(PIC2, 0x0B);
        if (!(inb(PIC2) & 0x80)) {
            outb(PIC1, 0x20);             /* master cascade için EOI */
            return 1;
        }
    }
    return 0;
}
