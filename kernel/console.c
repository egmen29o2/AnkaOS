/* AnkaOS - konsol katmanı: çıktı (VGA+seri), giriş kuyruğu, satır düzenleyici,
 * kprintf ve kernel panic. */
#include <kernel.h>

/* ---- çıktı ------------------------------------------------------------------
 * Yazma, en çok 80 karakterlik parçalar halinde ve kesmeler kapalıyken yapılır:
 * böylece birden çok görev (kabuk + arka plan programlar) aynı anda yazsa bile
 * bir satır ortasından bölünüp karışmaz. */
static void put_nolock(char c)
{
    vga_putc(c);
    if (c == '\n') serial_putc('\r');
    serial_putc(c);
}

void console_write(const char *s, int n)
{
    while (n > 0) {
        int chunk = n > 80 ? 80 : n;
        u32 fl = irq_save();
        for (int i = 0; i < chunk; i++) put_nolock(s[i]);
        vga_sync_cursor();
        irq_restore(fl);
        s += chunk;
        n -= chunk;
    }
}

void console_putc(char c) { console_write(&c, 1); }

void kputs(const char *s) { console_write(s, (int)strlen(s)); }

int kprintf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    console_write(buf, (int)strlen(buf));
    return n;
}

/* ---- giriş kuyruğu (klavye + seri IRQ'ları doldurur) --------------------------- */
#define INBUF_SZ 256
static volatile u16 inbuf[INBUF_SZ];
static volatile u32 in_head, in_tail;
static volatile int sigint_pending;

void console_raise_sigint(void) { sigint_pending = 1; }

int console_take_sigint(void)
{
    u32 fl = irq_save();
    int v = sigint_pending;
    sigint_pending = 0;
    irq_restore(fl);
    return v;
}

void input_push(u16 key)
{
    u32 next = (in_head + 1) % INBUF_SZ;
    if (next != in_tail) {
        inbuf[in_head] = key;
        in_head = next;
    }
    task_wake(WAIT_INPUT);
}

u16 input_get(void)
{
    for (;;) {
        u32 fl = irq_save();
        if (in_head != in_tail) {
            u16 k = inbuf[in_tail];
            in_tail = (in_tail + 1) % INBUF_SZ;
            irq_restore(fl);
            return k;
        }
        task_block(WAIT_INPUT);         /* kesmeler kapalıyken uyku -> kayıp uyanma yok */
        irq_restore(fl);
    }
}

void input_flush(void)
{
    u32 fl = irq_save();
    in_tail = in_head;
    irq_restore(fl);
}

/* ---- satır düzenleyici (+ geçmiş) ---------------------------------------------- */
#define HIST_N   8
#define HIST_LEN 128
static char hist[HIST_N][HIST_LEN];
static int hist_count;

static void hist_add(const char *line)
{
    if (hist_count && strcmp(hist[hist_count - 1], line) == 0) return;
    if (hist_count == HIST_N) {
        memmove(hist[0], hist[1], HIST_LEN * (HIST_N - 1));
        hist_count--;
    }
    strncpy(hist[hist_count], line, HIST_LEN - 1);
    hist[hist_count][HIST_LEN - 1] = 0;
    hist_count++;
}

static void erase_chars(int n)
{
    while (n-- > 0) console_write("\b \b", 3);
}

int console_readline(char *buf, int max, int use_hist)
{
    int len = 0, hpos = hist_count;
    buf[0] = 0;
    for (;;) {
        u16 k = input_get();
        if (k == '\n' || k == '\r') {
            console_putc('\n');
            buf[len] = 0;
            if (use_hist && len) hist_add(buf);
            return len;
        }
        if (k == 3)  { console_write("^C\n", 3); buf[0] = 0; return -1; }
        if (k == 4)  { if (len == 0) return -2; continue; }
        if (k == '\b' || k == 127) {
            if (len > 0) { len--; console_write("\b \b", 3); }
            continue;
        }
        if (k == 21) { erase_chars(len); len = 0; continue; }       /* Ctrl-U */
        if (use_hist && (k == KEY_UP || k == KEY_DOWN)) {
            if (k == KEY_UP && hpos > 0) hpos--;
            else if (k == KEY_DOWN && hpos < hist_count) hpos++;
            else continue;
            erase_chars(len);
            if (hpos < hist_count) {
                strncpy(buf, hist[hpos], (size_t)(max - 1));
                buf[max - 1] = 0;
                len = (int)strlen(buf);
            } else {
                len = 0;
            }
            console_write(buf, len);
            continue;
        }
        if (k >= 32 && k < 127 && len < max - 1) {
            buf[len++] = (char)k;
            console_putc((char)k);
        }
    }
}

/* ---- panic ve geri izleme ------------------------------------------------------- */
void backtrace(u32 ebp)
{
    kprintf("Backtrace:");
    for (int i = 0; i < 10 && ebp >= 0x10000 && ebp < 0x10000000 && !(ebp & 3); i++) {
        u32 *fp = (u32 *)ebp;
        if (!fp[1]) break;
        kprintf(" %p", fp[1]);
        ebp = fp[0];
    }
    kprintf("\n");
}

static volatile int panicking;

void kpanic(const char *fmt, ...)
{
    __asm__ volatile("cli");
    if (panicking) for (;;) __asm__ volatile("cli; hlt");
    panicking = 1;

    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    vga_recolor(0x4F);                  /* kırmızı zemin, beyaz yazı */
    kprintf("\n*** KERNEL PANIC: %s\n", buf);
    backtrace((u32)__builtin_frame_address(0));
    kprintf("Sistem durduruldu / System halted.\n");
    for (;;) __asm__ volatile("cli; hlt");
}
