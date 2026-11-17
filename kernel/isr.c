/* AnkaOS - kesme/istisna dağıtıcı (entry.S isr_common buraya gelir) */
#include <kernel.h>

static irq_handler_t irq_handlers[16];

void irq_install(int irq, irq_handler_t h) { irq_handlers[irq] = h; }

static const char *const exc_names[32] = {
    "Divide error", "Debug", "NMI", "Breakpoint", "Overflow", "Bound range exceeded",
    "Invalid opcode", "Device not available", "Double fault", "Coprocessor overrun",
    "Invalid TSS", "Segment not present", "Stack-segment fault", "General protection fault",
    "Page fault", "Reserved", "x87 FPU error", "Alignment check", "Machine check",
    "SIMD FP exception", "Virtualization", "Control protection", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved",
};

static void dump_regs(const struct regs *r)
{
    int user = (r->cs & 3) == 3;
    u32 esp = user ? r->useresp : (u32)&r->useresp;   /* kesilen ESP */
    kprintf("  eax=%08x ebx=%08x ecx=%08x edx=%08x\n", r->eax, r->ebx, r->ecx, r->edx);
    kprintf("  esi=%08x edi=%08x ebp=%08x esp=%08x\n", r->esi, r->edi, r->ebp, esp);
    kprintf("  eip=%08x cs=%04x eflags=%08x err=%08x\n", r->eip, r->cs & 0xFFFF, r->eflags, r->err_code);
    kprintf("  cr2=%08x cr3=%08x\n", read_cr2(), read_cr3());
}

static void exception(struct regs *r)
{
    u32 n = r->int_no;
    int user = (r->cs & 3) == 3;
    const char *name = n < 32 ? exc_names[n] : "Unknown";

    if (n == 14) {
        u32 addr = read_cr2();
        const char *kind = (r->err_code & 1) ? "protection violation" : "page not present";
        const char *acc  = (r->err_code & 2) ? "write" : "read";
        if (user) {
            kprintf("\n[pid %d '%s'] segmentation fault: %s at 0x%08x (%s), eip=0x%08x\n",
                    current->pid, current->name, acc, addr, kind, r->eip);
            task_exit(139);
        }
        vga_recolor(0x4F);
        kprintf("\n");
        dump_regs(r);
        kpanic("Page fault: %s at 0x%08x (%s) eip=0x%08x", acc, addr, kind, r->eip);
    }

    if (user) {
        kprintf("\n[pid %d '%s'] killed: %s (int %u, err 0x%x) at eip=0x%08x\n",
                current->pid, current->name, name, n, r->err_code, r->eip);
        task_exit(128 + (int)n);
    }

    vga_recolor(0x4F);
    kprintf("\n");
    dump_regs(r);
    kpanic("%s (int %u, err 0x%x) at eip=0x%08x", name, n, r->err_code, r->eip);
}

void isr_handler(struct regs *r)
{
    u32 n = r->int_no;

    if (n < 32) {
        exception(r);
    } else if (n < 48) {
        int irq = (int)n - 32;
        if ((irq == 7 || irq == 15) && pic_spurious(irq)) return;
        pic_eoi(irq);                          /* önce EOI: görev değişirse PIC tıkanmasın */
        if (irq_handlers[irq]) irq_handlers[irq](r);
        if (need_resched) {
            need_resched = 0;
            schedule();
        }
    } else if (n == 0x80) {
        syscall_handler(r);
    }
}
