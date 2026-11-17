/* AnkaOS - etkileşimli kabuk (çekirdek iş parçacığı olarak çalışır)
 * Yerleşik komutlar + /bin altındaki kullanıcı programları. '>' '>>' ile yönlendirme,
 * '&' ile arka plan, Yukarı/Aşağı ile geçmiş, Ctrl-C ile kesme. */
#include <kernel.h>

typedef struct { const char *name; int (*fn)(int, char **); const char *help; } cmd_t;

static fsnode_t *cwd;
static int g_bg;                                 /* satır '&' ile bitti mi */

/* ---- çıktı: konsol ya da '>' ile dosyaya ---------------------------------------- */
static char *redir_buf;
static u32 redir_len, redir_cap;
static int redirecting;

static void out_write(const char *s, int n)
{
    if (!redirecting) { console_write(s, n); return; }
    if (redir_len + (u32)n + 1 > redir_cap) {
        u32 nc = redir_cap ? redir_cap * 2 : 256;
        while (nc < redir_len + (u32)n + 1) nc *= 2;
        char *nb = krealloc(redir_buf, nc);
        if (!nb) return;
        redir_buf = nb;
        redir_cap = nc;
    }
    memcpy(redir_buf + redir_len, s, (size_t)n);
    redir_len += (u32)n;
}

static void outf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    out_write(buf, (int)strlen(buf));
}

/* ---- komutlar ---------------------------------------------------------------------- */
static int cmd_help(int, char **);    static int cmd_clear(int, char **);
static int cmd_echo(int, char **);    static int cmd_ls(int, char **);
static int cmd_cat(int, char **);     static int cmd_mkdir(int, char **);
static int cmd_touch(int, char **);   static int cmd_rm(int, char **);
static int cmd_cp(int, char **);      static int cmd_mv(int, char **);
static int cmd_cd(int, char **);      static int cmd_pwd(int, char **);
static int cmd_hexdump(int, char **); static int cmd_mem(int, char **);
static int cmd_uptime(int, char **);  static int cmd_date(int, char **);
static int cmd_ps(int, char **);      static int cmd_kill(int, char **);
static int cmd_run(int, char **);     static int cmd_sleep(int, char **);
static int cmd_threads(int, char **); static int cmd_cpu(int, char **);
static int cmd_color(int, char **);   static int cmd_uname(int, char **);
static int cmd_reboot(int, char **);  static int cmd_poweroff(int, char **);
static int cmd_panic(int, char **);   static int cmd_fault(int, char **);

static const cmd_t cmds[] = {
    { "help",     cmd_help,     "show this list" },
    { "clear",    cmd_clear,    "clear the screen" },
    { "echo",     cmd_echo,     "print text   (echo hi > file ; >> appends)" },
    { "ls",       cmd_ls,       "list a directory" },
    { "cat",      cmd_cat,      "print file contents" },
    { "mkdir",    cmd_mkdir,    "create directories" },
    { "touch",    cmd_touch,    "create empty files" },
    { "rm",       cmd_rm,       "remove files/dirs (-r = recursive)" },
    { "cp",       cmd_cp,       "copy a file" },
    { "mv",       cmd_mv,       "move / rename" },
    { "cd",       cmd_cd,       "change directory" },
    { "pwd",      cmd_pwd,      "print working directory" },
    { "hexdump",  cmd_hexdump,  "hex dump of a file: hexdump <file> [bytes]" },
    { "mem",      cmd_mem,      "memory usage" },
    { "uptime",   cmd_uptime,   "time since boot" },
    { "date",     cmd_date,     "RTC date/time (UTC)" },
    { "ps",       cmd_ps,       "list tasks" },
    { "kill",     cmd_kill,     "kill a task: kill <pid>" },
    { "run",      cmd_run,      "run a program: run <prog> [args] [&]" },
    { "sleep",    cmd_sleep,    "sleep N seconds" },
    { "threads",  cmd_threads,  "demo: 3 preemptive kernel threads" },
    { "cpu",      cmd_cpu,      "CPU information (cpuid)" },
    { "color",    cmd_color,    "color <fg 0-15> [bg 0-7]" },
    { "uname",    cmd_uname,    "system information" },
    { "reboot",   cmd_reboot,   "restart the machine" },
    { "poweroff", cmd_poweroff, "power off (QEMU/Bochs/VirtualBox)" },
    { "panic",    cmd_panic,    "test: trigger a kernel panic" },
    { "fault",    cmd_fault,    "test: kernel NULL dereference" },
};
#define NCMDS ((int)ARRAY_SIZE(cmds))

static int cmd_help(int argc, char **argv)
{
    (void)argc; (void)argv;
    outf("AnkaOS shell - built-in commands:\n");
    for (int i = 0; i < NCMDS; i++) outf("  %-9s %s\n", cmds[i].name, cmds[i].help);
    outf("Programs in /bin run by name:");
    for (int i = 0; i < nprogs; i++) outf(" %s", progs[i].name);
    outf("\n  add '&' to run in background. Keys: Up/Down history, Ctrl-C interrupt, Ctrl-U clear line.\n");
    return 0;
}

static int cmd_clear(int argc, char **argv) { (void)argc; (void)argv; vga_clear(); return 0; }

static int cmd_echo(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (i > 1) out_write(" ", 1);
        out_write(argv[i], (int)strlen(argv[i]));
    }
    out_write("\n", 1);
    return 0;
}

static int cmd_ls(int argc, char **argv)
{
    fsnode_t *d = argc > 1 ? fs_resolve(cwd, argv[1]) : cwd;
    if (!d) { outf("ls: %s: no such file or directory\n", argv[1]); return 1; }
    if (d->type == FS_FILE) { outf("  %6u  %s\n", d->size, d->name); return 0; }
    int n = 0;
    for (fsnode_t *c = d->child; c; c = c->next, n++) {
        if (c->type == FS_DIR) outf("  <dir>   %s/\n", c->name);
        else                   outf("  %6u  %s\n", c->size, c->name);
    }
    if (!n) outf("  (empty)\n");
    return 0;
}

static int cmd_cat(int argc, char **argv)
{
    if (argc < 2) { outf("usage: cat <file>...\n"); return 1; }
    for (int a = 1; a < argc; a++) {
        fsnode_t *f = fs_resolve(cwd, argv[a]);
        if (!f) { outf("cat: %s: no such file\n", argv[a]); return 1; }
        if (f->type != FS_FILE) { outf("cat: %s: is a directory\n", argv[a]); return 1; }
        char buf[128];
        u32 off = 0;
        int n;
        while ((n = fs_read(f, off, buf, sizeof buf)) > 0) {
            for (int i = 0; i < n; i++) {
                u8 c = (u8)buf[i];
                if ((c < 32 && c != '\n' && c != '\t') || c > 126) buf[i] = '.';
            }
            out_write(buf, n);
            off += (u32)n;
        }
    }
    return 0;
}

static int cmd_mkdir(int argc, char **argv)
{
    if (argc < 2) { outf("usage: mkdir <dir>...\n"); return 1; }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int r = fs_create(cwd, argv[i], FS_DIR, NULL);
        if (r) { outf("mkdir: %s: %s\n", argv[i], fs_strerror(r)); rc = 1; }
    }
    return rc;
}

static int cmd_touch(int argc, char **argv)
{
    if (argc < 2) { outf("usage: touch <file>...\n"); return 1; }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (fs_resolve(cwd, argv[i])) continue;
        int r = fs_create(cwd, argv[i], FS_FILE, NULL);
        if (r) { outf("touch: %s: %s\n", argv[i], fs_strerror(r)); rc = 1; }
    }
    return rc;
}

static int rm_tree(fsnode_t *n)
{
    if (n->type == FS_DIR)
        while (n->child) { int r = rm_tree(n->child); if (r) return r; }
    if (n == cwd) cwd = n->parent ? n->parent : fs_root();
    return fs_remove(n);
}

static int cmd_rm(int argc, char **argv)
{
    int rec = 0, first = 1, rc = 0;
    if (argc > 1 && strcmp(argv[1], "-r") == 0) { rec = 1; first = 2; }
    if (argc <= first) { outf("usage: rm [-r] <path>...\n"); return 1; }
    for (int i = first; i < argc; i++) {
        fsnode_t *n = fs_resolve(cwd, argv[i]);
        if (!n) { outf("rm: %s: no such file or directory\n", argv[i]); rc = 1; continue; }
        if (n->type == FS_DIR && !rec) { outf("rm: %s: is a directory (use -r)\n", argv[i]); rc = 1; continue; }
        int r = rm_tree(n);
        if (r) { outf("rm: %s: %s\n", argv[i], fs_strerror(r)); rc = 1; }
    }
    return rc;
}

static int cmd_cp(int argc, char **argv)
{
    if (argc != 3) { outf("usage: cp <src> <dst>\n"); return 1; }
    fsnode_t *src = fs_resolve(cwd, argv[1]);
    if (!src || src->type != FS_FILE) { outf("cp: %s: not a file\n", argv[1]); return 1; }
    char path[96];
    const char *dpath = argv[2];
    fsnode_t *dst = fs_resolve(cwd, dpath);
    if (dst && dst->type == FS_DIR) {
        ksnprintf(path, sizeof path, "%s/%s", argv[2], src->name);
        dpath = path;
        dst = fs_resolve(cwd, dpath);
    }
    if (dst == src) { outf("cp: source and destination are the same file\n"); return 1; }
    if (!dst) {
        int r = fs_create(cwd, dpath, FS_FILE, &dst);
        if (r) { outf("cp: %s: %s\n", dpath, fs_strerror(r)); return 1; }
    } else if (dst->type != FS_FILE) {
        outf("cp: %s: not a file\n", dpath);
        return 1;
    } else {
        fs_truncate(dst);
    }
    char buf[256];
    u32 off = 0;
    int n;
    while ((n = fs_read(src, off, buf, sizeof buf)) > 0) {
        if (fs_write(dst, off, buf, (u32)n) < 0) { outf("cp: out of memory\n"); return 1; }
        off += (u32)n;
    }
    return 0;
}

static int cmd_mv(int argc, char **argv)
{
    if (argc != 3) { outf("usage: mv <src> <dst>\n"); return 1; }
    fsnode_t *src = fs_resolve(cwd, argv[1]);
    if (!src) { outf("mv: %s: no such file or directory\n", argv[1]); return 1; }
    fsnode_t *dst = fs_resolve(cwd, argv[2]);
    int r;
    if (dst && dst->type == FS_DIR) {
        r = fs_move(src, dst, src->name);
    } else if (dst) {
        r = FS_EEXIST;
    } else {
        fsnode_t *dir;
        char name[FS_NAME_MAX + 1];
        r = fs_split(cwd, argv[2], &dir, name);
        if (!r) r = fs_move(src, dir, name);
    }
    if (r) { outf("mv: %s\n", fs_strerror(r)); return 1; }
    return 0;
}

static int cmd_cd(int argc, char **argv)
{
    fsnode_t *d = argc > 1 ? fs_resolve(cwd, argv[1]) : fs_root();
    if (!d) { outf("cd: %s: no such directory\n", argv[1]); return 1; }
    if (d->type != FS_DIR) { outf("cd: %s: not a directory\n", argv[1]); return 1; }
    cwd = d;
    return 0;
}

static int cmd_pwd(int argc, char **argv)
{
    (void)argc; (void)argv;
    char p[96];
    fs_path(cwd, p, sizeof p);
    outf("%s\n", p);
    return 0;
}

static int cmd_hexdump(int argc, char **argv)
{
    if (argc < 2) { outf("usage: hexdump <file> [bytes]\n"); return 1; }
    fsnode_t *f = fs_resolve(cwd, argv[1]);
    if (!f || f->type != FS_FILE) { outf("hexdump: %s: not a file\n", argv[1]); return 1; }
    u32 max = argc > 2 ? (u32)atoi(argv[2]) : 256;
    for (u32 off = 0; off < f->size && off < max; off += 16) {
        u8 row[16];
        int n = fs_read(f, off, row, 16);
        char line[96];
        int p = ksnprintf(line, sizeof line, "%08x  ", off);
        for (int i = 0; i < 16; i++) {
            if (i < n) p += ksnprintf(line + p, sizeof line - (size_t)p, "%02x ", row[i]);
            else       p += ksnprintf(line + p, sizeof line - (size_t)p, "   ");
        }
        p += ksnprintf(line + p, sizeof line - (size_t)p, " |");
        for (int i = 0; i < n; i++) line[p++] = (row[i] >= 32 && row[i] < 127) ? (char)row[i] : '.';
        line[p++] = '|';
        line[p++] = '\n';
        out_write(line, p);
    }
    return 0;
}

static int cmd_mem(int argc, char **argv)
{
    (void)argc; (void)argv;
    u32 tot = pmm_total_frames() * 4, fre = pmm_free_frames() * 4;
    u32 hu, hf, hb;
    heap_stats(&hu, &hf, &hb);
    outf("Physical memory : %u KB total, %u KB free, %u KB used\n", tot, fre, tot - fre);
    outf("Kernel heap     : %u KB used, %u KB free, %u blocks\n", hu / 1024, hf / 1024, hb);
    outf("Kernel image    : %u KB at 0x%x - 0x%x\n",
         ((u32)__kernel_end - (u32)__kernel_start) / 1024, (u32)__kernel_start, (u32)__kernel_end);
    outf("Page size       : %u bytes, identity map up to %u MB\n", PAGE_SIZE, pmm_mem_top() >> 20);
    return 0;
}

static int cmd_uptime(int argc, char **argv)
{
    (void)argc; (void)argv;
    u32 s = uptime_ms() / 1000;
    outf("up %u min %u s  (%u ticks @ %u Hz)\n", s / 60, s % 60, ticks, (u32)TIMER_HZ);
    return 0;
}

static u8 cmos(u8 reg) { outb(0x70, reg); return inb(0x71); }
static int unbcd(int v) { return (v & 0x0F) + ((v >> 4) * 10); }

static int cmd_date(int argc, char **argv)
{
    (void)argc; (void)argv;
    for (int i = 0; i < 100000 && (cmos(0x0A) & 0x80); i++) { }      /* güncelleme bitişini bekle */
    int sec = cmos(0x00), min = cmos(0x02), hour = cmos(0x04);
    int day = cmos(0x07), mon = cmos(0x08), year = cmos(0x09);
    u8 rb = cmos(0x0B);
    int pm = hour & 0x80;
    hour &= 0x7F;
    if (!(rb & 4)) {
        sec = unbcd(sec); min = unbcd(min); hour = unbcd(hour);
        day = unbcd(day); mon = unbcd(mon); year = unbcd(year);
    }
    if (!(rb & 2)) { hour %= 12; if (pm) hour += 12; }
    outf("20%02d-%02d-%02d %02d:%02d:%02d UTC\n", year, mon, day, hour, min, sec);
    return 0;
}

static int cmd_ps(int argc, char **argv)
{
    (void)argc; (void)argv;
    static const char *const st[] = { "unused", "ready", "running", "sleeping", "blocked", "zombie" };
    outf("  PID  STATE      CPU(ticks)  KIND    NAME\n");
    for (int i = 0; i < MAX_TASKS; i++) {
        task_t *t = task_at(i);
        if (!t) continue;
        outf("  %3d  %-9s  %10u  %-6s  %s\n", t->pid, st[t->state], t->cpu_ticks,
             t->is_user ? "user" : "kernel", t->name);
    }
    return 0;
}

static int cmd_kill(int argc, char **argv)
{
    if (argc != 2) { outf("usage: kill <pid>\n"); return 1; }
    int r = task_kill(atoi(argv[1]));
    if (r == -1) outf("kill: no such task\n");
    else if (r == -2) outf("kill: task is protected\n");
    return r ? 1 : 0;
}

#define EXEC_NOTFOUND (-1000)

static int exec_prog(int argc, char **argv, int bg)
{
    const char *name = argv[0];
    char path[64];
    fsnode_t *f;
    if (strchr(name, '/')) {
        f = fs_resolve(cwd, name);
    } else {
        ksnprintf(path, sizeof path, "/bin/%s", name);
        f = fs_resolve(fs_root(), path);
    }
    if (!f || f->type != FS_FILE) return EXEC_NOTFOUND;

    (void)console_take_sigint();                       /* eski Ctrl-C'yi unut */
    int pid = process_exec(name, f->data, f->size, argc, argv, bg);
    if (pid < 0) { outf("%s: cannot execute (error %d)\n", name, pid); return 126; }
    if (bg) { outf("[%d] %s started in background\n", pid, name); return 0; }

    int code = 0, killed = 0;
    for (;;) {
        int r = task_poll(pid, &code);
        if (r != 0) break;
        if (console_take_sigint() && !killed) { task_kill(pid); killed = 1; }
        task_sleep(10);
    }
    if (killed) input_flush();
    if (code) outf("[exit %d]\n", code);
    return code;
}

static int cmd_run(int argc, char **argv)
{
    if (argc < 2) { outf("usage: run <program> [args] [&]\n"); return 1; }
    int rc = exec_prog(argc - 1, argv + 1, g_bg);
    if (rc == EXEC_NOTFOUND) { outf("run: %s: program not found\n", argv[1]); return 127; }
    return rc;
}

static int cmd_sleep(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 1;
    (void)console_take_sigint();
    for (int i = 0; i < n * 100; i++) {
        task_sleep(10);
        if (console_take_sigint()) { outf("^C\n"); break; }
    }
    return 0;
}

static void demo_thread(void *arg)
{
    char id = (char)(u32)arg;
    for (int i = 1; i <= 5; i++) {
        kprintf("[thread %c] step %d  (tick %u)\n", id, i, ticks);
        task_sleep(150u + (u32)(id - 'A') * 100u);
    }
}

static int cmd_threads(int argc, char **argv)
{
    (void)argc; (void)argv;
    int pids[3];
    for (int i = 0; i < 3; i++) {
        char nm[16];
        ksnprintf(nm, sizeof nm, "thr-%c", 'A' + i);
        task_t *t = task_create(nm, demo_thread, (void *)(u32)('A' + i), 0, 0);
        pids[i] = t ? t->pid : -1;
    }
    (void)console_take_sigint();
    int left = 3;
    while (left) {
        left = 0;
        for (int i = 0; i < 3; i++) {
            if (pids[i] < 0) continue;
            int code;
            int r = task_poll(pids[i], &code);
            if (r == 0) left++; else pids[i] = -1;
        }
        if (console_take_sigint()) {
            for (int i = 0; i < 3; i++) if (pids[i] >= 0) task_kill(pids[i]);
        }
        if (left) task_sleep(20);
    }
    return 0;
}

static int cmd_cpu(int argc, char **argv)
{
    (void)argc; (void)argv;
    u32 a, b, c, d;
    char vendor[13];
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0), "c"(0));
    memcpy(vendor, &b, 4); memcpy(vendor + 4, &d, 4); memcpy(vendor + 8, &c, 4);
    vendor[12] = 0;
    outf("Vendor   : %s\n", vendor);

    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000000u), "c"(0));
    if (a >= 0x80000004u) {
        char brand[49];
        for (u32 i = 0; i < 3; i++) {
            u32 r[4];
            __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(0x80000002u + i), "c"(0));
            memcpy(brand + i * 16, r, 16);
        }
        brand[48] = 0;
        const char *p = brand;
        while (*p == ' ') p++;
        outf("Model    : %s\n", p);
    }
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    outf("Family   : %u  Model %u  Stepping %u\n", (a >> 8) & 0xF, (a >> 4) & 0xF, a & 0xF);
    outf("Features :");
    static const struct { u32 bit; const char *n; } feat[] = {
        {0, "fpu"}, {3, "pse"}, {4, "tsc"}, {5, "msr"}, {6, "pae"}, {9, "apic"},
        {15, "cmov"}, {23, "mmx"}, {25, "sse"}, {26, "sse2"},
    };
    for (u32 i = 0; i < ARRAY_SIZE(feat); i++)
        if (d & (1u << feat[i].bit)) outf(" %s", feat[i].n);
    outf("\n");
    return 0;
}

static int cmd_color(int argc, char **argv)
{
    if (argc < 2) { outf("usage: color <fg 0-15> [bg 0-7]\n"); return 1; }
    u8 fg = (u8)(atoi(argv[1]) & 15);
    u8 bg = argc > 2 ? (u8)(atoi(argv[2]) & 7) : 0;
    vga_recolor((u8)((bg << 4) | fg));
    return 0;
}

static int cmd_uname(int argc, char **argv)
{
    (void)argc; (void)argv;
    outf("%s %s i386 (built %s %s)\n", KERNEL_NAME, KERNEL_VERSION, __DATE__, __TIME__);
    return 0;
}

static int cmd_reboot(int argc, char **argv)
{
    (void)argc; (void)argv;
    kprintf("Rebooting...\n");
    for (int i = 0; i < 100000 && (inb(0x64) & 2); i++) { }
    outb(0x64, 0xFE);                                   /* klavye denetleyicisi reset hattı */
    struct { u16 limit; u32 base; } PACKED null_idt = { 0, 0 };
    __asm__ volatile("lidt %0; int3" : : "m"(null_idt));  /* olmadı: üçlü hata ile reset */
    for (;;) __asm__ volatile("cli; hlt");
    return 0;
}

static int cmd_poweroff(int argc, char **argv)
{
    (void)argc; (void)argv;
    kprintf("Powering off...\n");
    outw(0x604, 0x2000);                                /* QEMU (yeni) */
    outw(0xB004, 0x2000);                               /* Bochs / eski QEMU */
    outw(0x4004, 0x3400);                               /* VirtualBox */
    kprintf("It is now safe to turn off your computer.\n");
    for (;;) __asm__ volatile("cli; hlt");
    return 0;
}

static int cmd_panic(int argc, char **argv)
{
    (void)argc; (void)argv;
    kpanic("manual panic requested from the shell");
}

static int cmd_fault(int argc, char **argv)
{
    (void)argc; (void)argv;
    *(volatile u32 *)0 = 0xDEAD;                         /* NULL sayfası eşli değil -> #PF */
    return 0;
}

/* ---- ayrıştırma ve çalıştırma -------------------------------------------------------- */
static char tokbuf[320];

static int tokenize(const char *line, char **argv, int max)
{
    int argc = 0;
    char *out = tokbuf;
    const char *p = line;
    while (*p && argc < max - 1) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argv[argc++] = out;
        if (*p == '>') {
            *out++ = *p++;
            if (*p == '>') *out++ = *p++;
            *out++ = 0;
            continue;
        }
        int q = 0;
        while (*p && (q || (*p != ' ' && *p != '\t' && *p != '>'))) {
            if (*p == '"') { q = !q; p++; continue; }
            *out++ = *p++;
        }
        *out++ = 0;
    }
    argv[argc] = NULL;
    while (*p == ' ' || *p == '\t') p++;
    if (*p) return -1;                           /* kalan belirteç var: çok fazla argüman */
    return argc;
}

static void run_line(const char *line)
{
    char *argv[32];
    int argc = tokenize(line, argv, 32);
    if (argc < 0) { kprintf("anka: too many arguments (max 31)\n"); return; }
    if (!argc) return;

    g_bg = 0;
    if (argc > 1 && strcmp(argv[argc - 1], "&") == 0) { g_bg = 1; argv[--argc] = NULL; }

    const char *redir_file = NULL;
    int append = 0;
    for (int i = 0; i < argc;) {
        if (argv[i][0] == '>') {
            if (i + 1 >= argc) { kprintf("syntax error: file name expected after '>'\n"); return; }
            append = argv[i][1] == '>';
            redir_file = argv[i + 1];
            for (int j = i; j + 2 <= argc; j++) argv[j] = argv[j + 2];
            argc -= 2;
        } else {
            i++;
        }
    }
    if (!argc) return;

    redirecting = redir_file != NULL;
    redir_len = 0;

    const cmd_t *c = NULL;
    for (int i = 0; i < NCMDS; i++)
        if (strcmp(argv[0], cmds[i].name) == 0) { c = &cmds[i]; break; }

    if (c) {
        c->fn(argc, argv);
    } else if (exec_prog(argc, argv, g_bg) == EXEC_NOTFOUND) {
        outf("anka: command not found: %s  (type 'help')\n", argv[0]);
    }

    if (redirecting) {
        redirecting = 0;
        fsnode_t *f = fs_resolve(cwd, redir_file);
        if (!f) {
            int r = fs_create(cwd, redir_file, FS_FILE, &f);
            if (r) { kprintf("%s: %s\n", redir_file, fs_strerror(r)); return; }
        } else if (f->type != FS_FILE) {
            kprintf("%s: is a directory\n", redir_file);
            return;
        } else if (!append) {
            fs_truncate(f);
        }
        if (redir_len && fs_write(f, append ? f->size : 0, redir_buf, redir_len) < 0)
            kprintf("%s: write failed (out of memory)\n", redir_file);
    }
}

void shell_main(void *arg)
{
    (void)arg;
    cwd = fs_root();

    kprintf("\n");
    u8 a = vga_get_attr();
    vga_set_attr((u8)((a & 0xF0) | 0x0B));
    kprintf("  AnkaOS %s - type 'help' for commands\n", KERNEL_VERSION);
    vga_set_attr(a);
    fsnode_t *motd = fs_resolve(fs_root(), "/etc/motd");
    if (motd) {
        char buf[200];
        int n = fs_read(motd, 0, buf, sizeof buf - 1);
        if (n > 0) console_write(buf, n);
    }

    for (;;) {
        char path[96], line[128];
        fs_path(cwd, path, sizeof path);
        u8 cur = vga_get_attr();
        vga_set_attr((u8)((cur & 0xF0) | 0x0A));
        kprintf("anka:%s> ", path);
        vga_set_attr(cur);
        int n = console_readline(line, sizeof line, 1);
        if (n <= 0) continue;
        run_line(line);
    }
}
