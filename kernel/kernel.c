/* AnkaOS - çekirdek ana giriş noktası (entry.S _start -> kmain) */
#include <kernel.h>

static struct bootinfo bi;

static void step(const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    u8 a = vga_get_attr();
    kputs("[ ");
    vga_set_attr((u8)((a & 0xF0) | 0x0A));
    kputs("ok");
    vga_set_attr(a);
    kputs(" ] ");
    kputs(buf);
    kputs("\n");
}

void kmain(void)
{
    serial_init();
    vga_init();
    memcpy(&bi, (const void *)BOOTINFO_ADDR, sizeof bi);       /* sayfalamadan önce kopyala */

    kprintf("%s %s booting (i386, protected mode)\n", KERNEL_NAME, KERNEL_VERSION);
    if (bi.magic != BOOTINFO_MAGIC) kpanic("bad boot info magic 0x%08x", bi.magic);
    step("boot info: drive 0x%02x, %u E820 entries", bi.drive, bi.e820_count);

    gdt_init();
    step("GDT + TSS loaded (ring 0/3 segments)");
    idt_init();
    pic_init();
    step("IDT installed, PIC remapped to vectors 0x20-0x2F");

    pmm_init(&bi);
    step("physical memory: %u KB usable (%u frames)", pmm_total_frames() * 4, pmm_total_frames());

    paging_init();
    step("paging enabled, identity map %u MB, page 0 unmapped", pmm_mem_top() >> 20);

    u32 heap_bytes = (pmm_free_frames() / 4) * PAGE_SIZE;
    if (heap_bytes > 16u * 1024 * 1024) heap_bytes = 16u * 1024 * 1024;
    if (heap_bytes < 1024 * 1024) heap_bytes = 1024 * 1024;
    u32 heap_base = pmm_alloc_contig(heap_bytes / PAGE_SIZE);
    if (!heap_base) kpanic("cannot allocate kernel heap (%u KB)", heap_bytes / 1024);
    heap_init(heap_base, heap_bytes);
    step("kernel heap: %u KB at 0x%08x", heap_bytes / 1024, heap_base);

    task_init();
    pit_init(TIMER_HZ);
    step("PIT timer at %u Hz, preemptive scheduler ready", (u32)TIMER_HZ);

    kbd_init();
    serial_irq_init();
    step("PS/2 keyboard and COM1 serial input");

    fs_init();
    step("RAM filesystem mounted (%d programs in /bin)", nprogs);

    task_t *sh = task_create("shell", shell_main, NULL, 0, 1);
    if (!sh) kpanic("cannot start shell");
    sh->protect = 1;

    for (;;) __asm__ volatile("sti; hlt");                     /* bu bağlam artık "idle" görevi */
}
