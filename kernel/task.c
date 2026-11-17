/* AnkaOS - görev/işlem yöneticisi
 * - Önlemeli (preemptive) round-robin zamanlayıcı, 50 ms dilim
 * - Her görevin kendi çekirdek yığını vardır; context_switch (entry.S) yığın değiştirir
 * - Kullanıcı işlemleri: ayrı sayfa dizini + ring 3 (enter_user), hata olursa öldürülür */
#include <kernel.h>

#define KSTACK_SIZE  8192
#define TIME_SLICE   5                  /* tik cinsinden (5 x 10 ms) */
#define STACK_CANARY 0xC0DEFACEu

static task_t tasks[MAX_TASKS];
task_t *current;
volatile int need_resched;
static int next_pid = 1;

extern void context_switch(u32 *old_esp, u32 new_esp);
extern void enter_user(u32 entry, u32 user_esp);

void task_init(void)
{
    memset(tasks, 0, sizeof tasks);
    task_t *t = &tasks[0];                      /* kmain'in kendisi: "idle" görevi */
    t->pid = 0;
    strcpy(t->name, "idle");
    t->state = T_RUNNING;
    t->kstack_top = (u32)boot_stack_top;
    t->detached = 1;
    t->protect = 1;
    t->slice = TIME_SLICE;
    current = t;
}

static void reap(task_t *t)
{
    if (t->pd) vmm_destroy_space(t->pd);
    if (t->kstack) kfree((void *)t->kstack);
    memset(t, 0, sizeof *t);                    /* state = T_UNUSED */
}

static void reap_zombies(void)
{
    for (int i = 1; i < MAX_TASKS; i++) {
        task_t *t = &tasks[i];
        if (t->state == T_ZOMBIE && t != current && t->detached) reap(t);
    }
}

static task_t *pick_next(void)
{
    int cur = (int)(current - tasks);
    for (int i = 1; i <= MAX_TASKS; i++) {
        int idx = (cur + i) % MAX_TASKS;
        if (idx == 0) continue;                 /* idle yalnızca başka iş yoksa */
        if (tasks[idx].state == T_RUNNABLE) return &tasks[idx];
    }
    return &tasks[0];
}

void schedule(void)
{
    u32 fl = irq_save();
    task_t *prev = current;
    reap_zombies();
    if (prev->state == T_RUNNING) prev->state = T_RUNNABLE;
    task_t *next = pick_next();
    next->state = T_RUNNING;
    next->slice = TIME_SLICE;
    if (next != prev) {
        current = next;
        tss_set_kstack(next->kstack_top);
        u32 pd = next->pd ? next->pd : vmm_kernel_pd();
        if (pd != vmm_current()) vmm_switch(pd);
        if (prev->kstack && *(u32 *)prev->kstack != STACK_CANARY)
            kpanic("kernel stack overflow in task %d (%s)", prev->pid, prev->name);
        context_switch(&prev->esp, next->esp);
    }
    irq_restore(fl);
}

void task_yield(void) { schedule(); }

/* yeni görevin ilk çalıştığı yer: context_switch 'ret' ile buraya düşer */
static void task_bootstrap(void)
{
    __asm__ volatile("sti");
    task_t *t = current;
    t->entry(t->arg);
    task_exit(0);
}

task_t *task_create(const char *name, void (*entry)(void *), void *arg, u32 pd, int detached)
{
    u32 fl = irq_save();
    task_t *t = NULL;
    for (int i = 1; i < MAX_TASKS; i++)
        if (tasks[i].state == T_UNUSED) { t = &tasks[i]; break; }
    u8 *stack = t ? kmalloc(KSTACK_SIZE) : NULL;
    if (!t || !stack) { irq_restore(fl); return NULL; }

    memset(t, 0, sizeof *t);
    *(u32 *)stack = STACK_CANARY;
    t->kstack = (u32)stack;
    t->kstack_top = (u32)stack + KSTACK_SIZE;

    u32 *sp = (u32 *)t->kstack_top;
    *--sp = 0;                                  /* sahte dönüş adresi (bootstrap dönmez) */
    *--sp = (u32)task_bootstrap;                /* context_switch'in 'ret' hedefi */
    *--sp = 0;                                  /* ebp */
    *--sp = 0;                                  /* ebx */
    *--sp = 0;                                  /* esi */
    *--sp = 0;                                  /* edi */
    t->esp = (u32)sp;

    t->pid = next_pid++;
    strncpy(t->name, name, sizeof t->name - 1);
    t->pd = pd;
    t->detached = detached;
    t->entry = entry;
    t->arg = arg;
    t->start_tick = ticks;
    t->state = T_RUNNABLE;
    irq_restore(fl);
    return t;
}

NORETURN void task_exit(int code)
{
    __asm__ volatile("cli");
    current->exit_code = code;
    current->state = T_ZOMBIE;
    schedule();
    kpanic("zombie task %d was scheduled", current->pid);
}

int task_kill(int pid)
{
    u32 fl = irq_save();
    for (int i = 1; i < MAX_TASKS; i++) {
        task_t *t = &tasks[i];
        if (t->state == T_UNUSED || t->state == T_ZOMBIE || t->pid != pid) continue;
        if (t->protect) { irq_restore(fl); return -2; }
        if (t == current) { irq_restore(fl); task_exit(137); }
        t->exit_code = 137;
        t->state = T_ZOMBIE;
        irq_restore(fl);
        return 0;
    }
    irq_restore(fl);
    return -1;
}

/* 1 = bitti (zombi toplandı, kod verildi), 0 = çalışıyor, -1 = böyle bir görev yok */
int task_poll(int pid, int *code)
{
    u32 fl = irq_save();
    for (int i = 1; i < MAX_TASKS; i++) {
        task_t *t = &tasks[i];
        if (t->state == T_UNUSED || t->pid != pid) continue;
        if (t->state == T_ZOMBIE && t != current) {
            if (code) *code = t->exit_code;
            reap(t);
            irq_restore(fl);
            return 1;
        }
        irq_restore(fl);
        return 0;
    }
    irq_restore(fl);
    return -1;
}

task_t *task_at(int idx)
{
    if (idx < 0 || idx >= MAX_TASKS || tasks[idx].state == T_UNUSED) return NULL;
    return &tasks[idx];
}

void task_sleep(u32 ms)
{
    u32 t = (ms + MS_PER_TICK - 1) / MS_PER_TICK;
    if (t == 0) { task_yield(); return; }
    u32 fl = irq_save();
    current->wake_tick = ticks + t;
    current->state = T_SLEEPING;
    schedule();
    irq_restore(fl);
}

void task_block(int reason)
{
    u32 fl = irq_save();
    current->state = T_BLOCKED;
    current->wait_reason = reason;
    schedule();
    irq_restore(fl);
}

void task_wake(int reason)
{
    u32 fl = irq_save();
    for (int i = 1; i < MAX_TASKS; i++) {
        task_t *t = &tasks[i];
        if (t->state == T_BLOCKED && t->wait_reason == reason) {
            t->state = T_RUNNABLE;
            need_resched = 1;
        }
    }
    irq_restore(fl);
}

/* PIT kesmesinden her tikte çağrılır */
void task_tick(void)
{
    for (int i = 1; i < MAX_TASKS; i++) {
        task_t *t = &tasks[i];
        if (t->state == T_SLEEPING && (s32)(ticks - t->wake_tick) >= 0)
            t->state = T_RUNNABLE;
    }
    current->cpu_ticks++;
    if (current == &tasks[0]) need_resched = 1;         /* idle: iş varsa hemen geç */
    else if (--current->slice <= 0) need_resched = 1;
}

/* ---- kullanıcı işlemleri -------------------------------------------------- */
static void user_start(void *arg)
{
    (void)arg;
    enter_user(current->user_entry, current->user_esp);
}

int process_exec(const char *name, const u8 *img, u32 size, int argc, char **argv, int detached)
{
    if (size < sizeof(struct exec_header)) return -1;
    struct exec_header h;
    memcpy(&h, img, sizeof h);
    if (h.magic != EXEC_MAGIC) return -2;
    if (h.memsz < size || h.memsz > 4u * 1024 * 1024) return -3;
    if (h.entry < USER_BASE || h.entry >= USER_BASE + h.memsz) return -3;

    u32 pd = vmm_create_space();
    if (!pd) return -4;

    /* program imajı: dosya içeriği + sıfırlanmış bss */
    u32 npages = (h.memsz + PAGE_SIZE - 1) / PAGE_SIZE;
    for (u32 i = 0; i < npages; i++) {
        u32 f = pmm_alloc_zeroed();
        if (!f) { vmm_destroy_space(pd); return -4; }
        if (vmm_map(pd, USER_BASE + i * PAGE_SIZE, f, PTE_RW | PTE_US) < 0) {
            pmm_free(f);
            vmm_destroy_space(pd);
            return -4;
        }
        u32 off = i * PAGE_SIZE;
        if (off < size) {
            u32 n = size - off;
            if (n > PAGE_SIZE) n = PAGE_SIZE;
            memcpy((void *)f, img + off, n);
        }
    }

    /* yığın: USER_STACK_PAGES sayfa, en üst sayfaya argv yazılır */
    u32 top_frame = 0;
    for (int i = 0; i < USER_STACK_PAGES; i++) {
        u32 f = pmm_alloc_zeroed();
        if (!f) { vmm_destroy_space(pd); return -4; }
        if (vmm_map(pd, USER_STACK_TOP - (u32)(i + 1) * PAGE_SIZE, f, PTE_RW | PTE_US) < 0) {
            pmm_free(f);
            vmm_destroy_space(pd);
            return -4;
        }
        if (i == 0) top_frame = f;
    }

    u32 base = USER_STACK_TOP - PAGE_SIZE;      /* en üst sayfanın sanal başlangıcı */
    u32 sp = USER_STACK_TOP;
    u32 uargv[9];
    if (argc > 8) argc = 8;
    for (int i = argc - 1; i >= 0; i--) {
        u32 len = (u32)strlen(argv[i]);
        if (len > 100) len = 100;
        sp -= len + 1;
        char *dst = (char *)(top_frame + (sp - base));
        memcpy(dst, argv[i], len);
        dst[len] = 0;
        uargv[i] = sp;
    }
    sp &= ~3u;
    uargv[argc] = 0;
    sp -= 4 * (u32)(argc + 1);
    u32 argv_addr = sp;
    memcpy((void *)(top_frame + (sp - base)), uargv, 4 * (u32)(argc + 1));
    sp -= 4; *(u32 *)(top_frame + (sp - base)) = argv_addr;     /* argv */
    sp -= 4; *(u32 *)(top_frame + (sp - base)) = (u32)argc;     /* argc */

    u32 fl = irq_save();                        /* görev çalışmaya başlamadan alanları doldur */
    task_t *t = task_create(name, user_start, NULL, pd, detached);
    if (!t) { irq_restore(fl); vmm_destroy_space(pd); return -5; }
    t->is_user = 1;
    t->user_entry = h.entry;
    t->user_esp = sp;
    int pid = t->pid;
    irq_restore(fl);
    return pid;
}
