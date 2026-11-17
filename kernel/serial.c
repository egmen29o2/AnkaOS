/* AnkaOS - COM1 seri port sürücüsü (115200 8N1). Günlük + giriş (IRQ4). */
#include <kernel.h>

#define COM1 0x3F8

void serial_init(void)
{
    outb(COM1 + 1, 0x00);       /* kesmeleri kapat            */
    outb(COM1 + 3, 0x80);       /* DLAB aç                    */
    outb(COM1 + 0, 0x01);       /* bölen = 1 -> 115200 baud   */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);       /* 8 bit, parity yok, 1 stop  */
    outb(COM1 + 2, 0xC7);       /* FIFO aç ve temizle         */
    outb(COM1 + 4, 0x0B);       /* DTR + RTS + OUT2           */
}

void serial_putc(char c)
{
    for (int t = 0; t < 100000 && !(inb(COM1 + 5) & 0x20); t++) { }
    outb(COM1, (u8)c);
}

static int esc_state;           /* 0 normal, 1 ESC görüldü, 2 ESC [ görüldü */

static void serial_irq(struct regs *r)
{
    (void)r;
    for (int n = 0; n < 64 && (inb(COM1 + 5) & 1); n++) {
        u8 c = inb(COM1);
        if (esc_state == 1) { esc_state = (c == '[') ? 2 : 0; continue; }
        if (esc_state == 2) {
            esc_state = 0;
            switch (c) {
            case 'A': input_push(KEY_UP); break;
            case 'B': input_push(KEY_DOWN); break;
            case 'C': input_push(KEY_RIGHT); break;
            case 'D': input_push(KEY_LEFT); break;
            }
            continue;
        }
        if (c == 27) { esc_state = 1; continue; }
        if (c == '\r') c = '\n';
        else if (c == 0x7F) c = '\b';
        if (c == 3) console_raise_sigint();
        input_push(c);
    }
}

void serial_irq_init(void)
{
    irq_install(4, serial_irq);
    outb(COM1 + 1, 0x01);       /* "veri alındı" kesmesi */
    irq_unmask(4);
}
