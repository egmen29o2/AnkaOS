/* AnkaOS - 8253/8254 PIT: sistem zamanlayıcısı (kanal 0, TIMER_HZ) */
#include <kernel.h>

volatile u32 ticks;

static void pit_irq(struct regs *r)
{
    (void)r;
    ticks++;
    task_tick();
}

void pit_init(u32 hz)
{
    u32 div = 1193182 / hz;
    outb(0x43, 0x36);                     /* kanal 0, lo/hi, mod 3 (kare dalga) */
    outb(0x40, (u8)(div & 0xFF));
    outb(0x40, (u8)((div >> 8) & 0xFF));
    irq_install(0, pit_irq);
    irq_unmask(0);
}

u32 uptime_ms(void) { return ticks * MS_PER_TICK; }
