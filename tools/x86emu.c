/* ===========================================================================
 * x86emu - AnkaOS için minimal i386 PC emülatörü (QEMU olmadan duman testi)
 *
 * Gerçek bir PC'yi taklit etmez; yalnızca AnkaOS'un kullandığı alt kümeyi uygular:
 *   - gerçek mod (önyükleyici) + BIOS INT 10h/13h/15h taklidi (HLE)
 *   - korumalı mod, GDT/IDT/TSS, ring 0/3 geçişleri, sayfalama (4 KB), istisnalar
 *   - 8259 PIC x2, 8254 PIT (kanal 0), PS/2 klavye, COM1, CMOS RTC, VGA metin belleği
 * Kullanım:  x86emu [-n max_komut] os.img script.txt
 * Script komutları:  expect <metin> | send <metin> | serial <metin> | wait <ms> | screen | quit
 * =========================================================================== */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <setjmp.h>

typedef uint8_t u8;  typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int8_t  s8;  typedef int16_t  s16; typedef int32_t  s32; typedef int64_t  s64;

enum { EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI };
enum { ES, CS, SS, DS, FS, GS };
#define CF 0x0001u
#define PF 0x0004u
#define AF 0x0010u
#define ZF 0x0040u
#define SF 0x0080u
#define TF 0x0100u
#define IF 0x0200u
#define DF 0x0400u
#define OF 0x0800u
#define MASK(sz) ((sz) == 4 ? 0xFFFFFFFFu : (sz) == 2 ? 0xFFFFu : 0xFFu)
#define SIGN(sz) ((sz) == 4 ? 0x80000000u : (sz) == 2 ? 0x8000u : 0x80u)

typedef struct { u16 sel; u32 base, limit; u8 access; int d, present, dpl; } Seg;

static struct {
    u32 r[8], eip, eflags;
    Seg s[6];
    u32 cr0, cr2, cr3, cr4;
    u32 gdt_base, gdt_limit, idt_base, idt_limit;
    u32 tr_base;
    int cpl, halted, shadow;
    u64 icount;
} C;

static u8 *ram;
static u32 ram_size = 128u << 20;
static u8 *disk;
static u32 disk_size;
static jmp_buf jb;
static int x_vec, x_err, x_has, in_deliver, g_super, stop_reason;
static u32 snap_r[8], snap_eip, snap_eflags;
static u32 trace[32];
static int trace_i;
enum { RUNNING = 0, STOP_POWEROFF = 1, STOP_RESET = 2, STOP_FATAL = 3 };

/* ---- yardımcılar ------------------------------------------------------------ */
static void dump_state(void)
{
    fprintf(stderr, "EAX=%08x ECX=%08x EDX=%08x EBX=%08x\nESP=%08x EBP=%08x ESI=%08x EDI=%08x\n",
            C.r[EAX], C.r[ECX], C.r[EDX], C.r[EBX], C.r[ESP], C.r[EBP], C.r[ESI], C.r[EDI]);
    fprintf(stderr, "EIP=%08x EFLAGS=%08x CPL=%d CS=%04x SS=%04x DS=%04x CR0=%08x CR2=%08x CR3=%08x icount=%llu\n",
            C.eip, C.eflags, C.cpl, C.s[CS].sel, C.s[SS].sel, C.s[DS].sel, C.cr0, C.cr2, C.cr3,
            (unsigned long long)C.icount);
    fprintf(stderr, "son komutlar (EIP):");
    for (int i = 0; i < 32; i++) fprintf(stderr, " %x", trace[(trace_i + i) & 31]);
    fprintf(stderr, "\n");
}

static void die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
#include <stdarg.h>
static void die(const char *fmt, ...)
{
    va_list ap;
    fflush(stdout);
    fprintf(stderr, "\n*** EMULATOR: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    dump_state();
    exit(2);
}

static void raise_x(int vec, int err, int has) __attribute__((noreturn));
static void raise_x(int vec, int err, int has) { x_vec = vec; x_err = err; x_has = has; longjmp(jb, 1); }

/* ---- fiziksel/lineer bellek --------------------------------------------------------- */
static u32 phys_rd(u32 pa, int sz)
{
    u32 v = 0;
    if ((u64)pa + sz > ram_size) return MASK(sz);
    memcpy(&v, ram + pa, sz);
    return v;
}
static void phys_wr(u32 pa, u32 v, int sz)
{
    if ((u64)pa + sz > ram_size) return;
    memcpy(ram + pa, &v, sz);
}

static u32 translate(u32 lin, int wr)
{
    if (!(C.cr0 & 0x80000000u)) return lin;
    int user = (C.cpl == 3) && !g_super;
    u32 pde_a = (C.cr3 & ~0xFFFu) + ((lin >> 22) << 2);
    u32 pde = phys_rd(pde_a, 4);
    if (!(pde & 1)) { C.cr2 = lin; raise_x(14, (wr ? 2 : 0) | (user ? 4 : 0), 1); }
    if (pde & 0x80) die("PSE sayfa emulatorde desteklenmiyor");
    u32 pte_a = (pde & ~0xFFFu) + (((lin >> 12) & 0x3FF) << 2);
    u32 pte = phys_rd(pte_a, 4);
    if (!(pte & 1)) { C.cr2 = lin; raise_x(14, (wr ? 2 : 0) | (user ? 4 : 0), 1); }
    int us = (pde & pte & 4) != 0, rw = (pde & pte & 2) != 0;
    if ((user && !us) || (wr && !rw && (user || (C.cr0 & 0x10000u)))) {
        C.cr2 = lin;
        raise_x(14, 1 | (wr ? 2 : 0) | (user ? 4 : 0), 1);
    }
    if (!(pde & 0x20)) phys_wr(pde_a, pde | 0x20, 4);
    u32 npte = pte | 0x20 | (wr ? 0x40 : 0);
    if (npte != pte) phys_wr(pte_a, npte, 4);
    return (pte & ~0xFFFu) | (lin & 0xFFF);
}

static u32 mem_rd(u32 lin, int sz)
{
    if (sz > 1 && ((lin & 0xFFF) + sz) > 0x1000) {
        u32 v = 0;
        for (int i = 0; i < sz; i++) v |= mem_rd(lin + i, 1) << (8 * i);
        return v;
    }
    return phys_rd(translate(lin, 0), sz);
}
static void mem_wr(u32 lin, u32 v, int sz)
{
    if (sz > 1 && ((lin & 0xFFF) + sz) > 0x1000) {
        for (int i = 0; i < sz; i++) mem_wr(lin + i, (v >> (8 * i)) & 0xFF, 1);
        return;
    }
    phys_wr(translate(lin, 1), v, sz);
}
static u32 sup_rd(u32 lin, int sz)
{
    int s = g_super; g_super = 1;
    u32 v = mem_rd(lin, sz);
    g_super = s;
    return v;
}

/* ---- yazmaçlar ------------------------------------------------------------------------ */
static u32 rd_reg(int r, int sz)
{
    if (sz == 4) return C.r[r];
    if (sz == 2) return C.r[r] & 0xFFFF;
    return r < 4 ? (C.r[r] & 0xFF) : ((C.r[r - 4] >> 8) & 0xFF);
}
static void wr_reg(int r, int sz, u32 v)
{
    if (sz == 4) C.r[r] = v;
    else if (sz == 2) C.r[r] = (C.r[r] & 0xFFFF0000u) | (v & 0xFFFF);
    else if (r < 4) C.r[r] = (C.r[r] & 0xFFFFFF00u) | (v & 0xFF);
    else C.r[r - 4] = (C.r[r - 4] & 0xFFFF00FFu) | ((v & 0xFF) << 8);
}

static void set_flag(u32 f, int on) { if (on) C.eflags |= f; else C.eflags &= ~f; }
static void set_szp(u32 r, int sz)
{
    r &= MASK(sz);
    set_flag(ZF, r == 0);
    set_flag(SF, (r & SIGN(sz)) != 0);
    set_flag(PF, !__builtin_parity(r & 0xFF));
}

/* ---- bölüt (segment) yükleme ---------------------------------------------------------- */
typedef struct { u32 base, limit; u8 access; int d, dpl; } Desc;

static int read_desc(u16 sel, Desc *d)
{
    u32 idx = sel & 0xFFF8u;
    if (sel & 4) die("LDT desteklenmiyor");
    if (idx + 7 > C.gdt_limit) return -1;
    u32 lo = sup_rd(C.gdt_base + idx, 4), hi = sup_rd(C.gdt_base + idx + 4, 4);
    d->base = (lo >> 16) | ((hi & 0xFF) << 16) | (hi & 0xFF000000u);
    d->limit = (lo & 0xFFFF) | (hi & 0x000F0000u);
    if (hi & 0x00800000u) d->limit = (d->limit << 12) | 0xFFF;
    d->access = (u8)(hi >> 8);
    d->d = (hi >> 22) & 1;
    d->dpl = (d->access >> 5) & 3;
    return 0;
}

static void load_seg(int sr, u16 sel)
{
    Seg *s = &C.s[sr];
    if (!(C.cr0 & 1)) {                                  /* gerçek mod */
        s->sel = sel; s->base = (u32)sel << 4; s->limit = 0xFFFF; s->present = 1; s->dpl = 0;
        return;
    }
    if ((sel & 0xFFFC) == 0) {
        if (sr == SS || sr == CS) raise_x(13, 0, 1);
        s->sel = sel; s->present = 0; s->base = 0; s->limit = 0;
        return;
    }
    Desc d;
    if (read_desc(sel, &d) < 0) raise_x(13, sel & 0xFFFC, 1);
    int rpl = sel & 3;
    if (!(d.access & 0x10)) raise_x(13, sel & 0xFFFC, 1);            /* sistem bölütü olamaz */
    if (sr == SS) {
        if ((d.access & 0x08) || !(d.access & 0x02) || rpl != C.cpl || d.dpl != C.cpl) raise_x(13, sel & 0xFFFC, 1);
        if (!(d.access & 0x80)) raise_x(12, sel & 0xFFFC, 1);
    } else {
        if ((d.access & 0x08) && !(d.access & 0x02)) raise_x(13, sel & 0xFFFC, 1);   /* okunamaz kod */
        int conforming = (d.access & 0x08) && (d.access & 0x04);
        if (!conforming && (d.dpl < C.cpl || d.dpl < rpl)) raise_x(13, sel & 0xFFFC, 1);
        if (!(d.access & 0x80)) raise_x(11, sel & 0xFFFC, 1);
    }
    s->sel = sel; s->base = d.base; s->limit = d.limit; s->access = d.access; s->d = d.d;
    s->present = 1; s->dpl = d.dpl;
}

static void load_cs(u16 sel, int new_cpl)
{
    Seg *s = &C.s[CS];
    if (!(C.cr0 & 1)) {
        s->sel = sel; s->base = (u32)sel << 4; s->limit = 0xFFFF; s->d = 0; s->present = 1; s->dpl = 0;
        C.cpl = 0;
        return;
    }
    Desc d;
    if (read_desc(sel, &d) < 0) raise_x(13, sel & 0xFFFC, 1);
    if ((d.access & 0x18) != 0x18) raise_x(13, sel & 0xFFFC, 1);        /* kod bölütü olmalı */
    if (!(d.access & 0x80)) raise_x(11, sel & 0xFFFC, 1);
    s->sel = (u16)((sel & 0xFFFC) | new_cpl); s->base = d.base; s->limit = d.limit;
    s->access = d.access; s->d = d.d; s->present = 1; s->dpl = d.dpl;
    C.cpl = new_cpl;
}

/* ---- yığın ------------------------------------------------------------------------------ */
static void set_sp(u32 sp, u32 mask) { C.r[ESP] = (mask == 0xFFFFu) ? ((C.r[ESP] & 0xFFFF0000u) | (sp & 0xFFFF)) : sp; }
static void push(u32 v, int sz)
{
    u32 mask = C.s[SS].d ? 0xFFFFFFFFu : 0xFFFFu;
    u32 sp = (C.r[ESP] - sz) & mask;
    mem_wr(C.s[SS].base + sp, v, sz);
    set_sp(sp, mask);
}
static u32 pop(int sz)
{
    u32 mask = C.s[SS].d ? 0xFFFFFFFFu : 0xFFFFu;
    u32 sp = C.r[ESP] & mask;
    u32 v = mem_rd(C.s[SS].base + sp, sz);
    set_sp((sp + sz) & mask, mask);
    return v;
}

/* ---- 8259 PIC, PIT, klavye, seri, CMOS ---------------------------------------------------- */
typedef struct { u8 irr, isr, imr, base, icw1; int step, read_isr; } PIC;
static PIC pic[2];
static int pit_on, pit_lohi, pit_reload_lo;
static u64 pit_next, pit_period;
static u8 kbd_q[65536]; static int kbd_h, kbd_t;
static u8 ser_rx[1024]; static int ser_h, ser_t; static u8 ser_ier, ser_lcr;
static u8 crtc_idx, crtc_reg[32], cmos_idx;
static char *serial_log; static size_t serial_len, serial_cap;

static void pic_raise(int irq) { if (irq < 8) pic[0].irr |= 1u << irq; else pic[1].irr |= 1u << (irq - 8); }

static int pic_pick(PIC *p, u8 pending)
{
    for (int i = 0; i < 8; i++) {
        if (p->isr & (1u << i)) return -1;
        if (pending & (1u << i)) return i;
    }
    return -1;
}

static int pic_ack(void)
{
    u8 mp = pic[0].irr & ~pic[0].imr;
    u8 sp = pic[1].irr & ~pic[1].imr;
    if (pic_pick(&pic[1], sp) >= 0 && !(pic[0].isr & 4)) mp |= 4;
    int ir = pic_pick(&pic[0], mp);
    if (ir < 0) return -1;
    if (ir == 2) {
        int s = pic_pick(&pic[1], sp);
        if (s < 0) return -1;
        pic[1].irr &= ~(1u << s); pic[1].isr |= 1u << s; pic[0].isr |= 4;
        return pic[1].base + s;
    }
    pic[0].irr &= ~(1u << ir); pic[0].isr |= 1u << ir;
    return pic[0].base + ir;
}

static void pic_write(int n, int port1, u8 v)
{
    PIC *p = &pic[n];
    if (!port1) {
        if (v & 0x10) { p->icw1 = v; p->step = 1; p->imr = 0; p->isr = 0; p->irr = 0; }
        else if (v & 0x08) { if ((v & 3) == 2) p->read_isr = 0; else if ((v & 3) == 3) p->read_isr = 1; }
        else if (v & 0x20) {
            if (v & 0x40) p->isr &= ~(1u << (v & 7));
            else for (int i = 0; i < 8; i++) if (p->isr & (1u << i)) { p->isr &= ~(1u << i); break; }
        }
    } else if (p->step == 1) { p->base = v & 0xF8; p->step = (p->icw1 & 2) ? ((p->icw1 & 1) ? 3 : 0) : 2; }
    else if (p->step == 2) { p->step = (p->icw1 & 1) ? 3 : 0; }
    else if (p->step == 3) { p->step = 0; }
    else p->imr = v;
}

static void kbd_push(u8 sc) { kbd_q[kbd_t++ & 65535] = sc; if (kbd_t - kbd_h == 1) pic_raise(1); }
static void ser_push(u8 c)  { ser_rx[ser_t++ & 1023] = c; if ((ser_t - ser_h) == 1 && (ser_ier & 1)) pic_raise(4); }

static void serial_out(u8 c)
{
    if (serial_len + 2 > serial_cap) { serial_cap = serial_cap ? serial_cap * 2 : 65536; serial_log = realloc(serial_log, serial_cap); }
    serial_log[serial_len++] = (char)c;
    serial_log[serial_len] = 0;
    if (c != '\r') putchar(c);
}

static u32 port_in(u16 p, int sz)
{
    switch (p) {
    case 0x20: return pic[0].read_isr ? pic[0].isr : pic[0].irr;
    case 0xA0: return pic[1].read_isr ? pic[1].isr : pic[1].irr;
    case 0x21: return pic[0].imr;
    case 0xA1: return pic[1].imr;
    case 0x60: {
        u8 v = (kbd_t != kbd_h) ? kbd_q[kbd_h++ & 65535] : 0;
        if (kbd_t != kbd_h) pic_raise(1);
        return v;
    }
    case 0x64: return (kbd_t != kbd_h ? 1 : 0) | 4;
    case 0x71: {
        static const u8 cm[16] = { 0x56, 0, 0x34, 0, 0x12, 0, 0, 0x15, 0x11, 0x26, 0x00, 0x02, 0, 0, 0, 0 };
        return (cmos_idx & 0x7F) < 16 ? cm[cmos_idx & 0x7F] : 0;
    }
    case 0x3D5: return crtc_reg[crtc_idx & 31];
    case 0x3F8: {
        u8 v = (ser_t != ser_h) ? ser_rx[ser_h++ & 1023] : 0;
        if (ser_t != ser_h && (ser_ier & 1)) pic_raise(4);
        return v;
    }
    case 0x3F9: return ser_ier;
    case 0x3FA: return (ser_t != ser_h && (ser_ier & 1)) ? 0x04 : 0x01;
    case 0x3FB: return ser_lcr;
    case 0x3FD: return 0x60 | (ser_t != ser_h ? 1 : 0);
    case 0x92:  return 0x02;
    default: (void)sz; return 0xFFFFFFFFu;
    }
}

static void port_out(u16 p, u32 v, int sz)
{
    (void)sz;
    switch (p) {
    case 0x20: pic_write(0, 0, (u8)v); break;
    case 0x21: pic_write(0, 1, (u8)v); break;
    case 0xA0: pic_write(1, 0, (u8)v); break;
    case 0xA1: pic_write(1, 1, (u8)v); break;
    case 0x40:
        if (!pit_lohi) { pit_reload_lo = v & 0xFF; pit_lohi = 1; }
        else {
            u32 rl = pit_reload_lo | ((v & 0xFF) << 8);
            if (!rl) rl = 65536;
            pit_period = rl; pit_on = 1; pit_next = C.icount + pit_period; pit_lohi = 0;
        }
        break;
    case 0x43: pit_lohi = 0; break;
    case 0x64: if ((v & 0xFF) == 0xFE) stop_reason = STOP_RESET; break;
    case 0x70: cmos_idx = (u8)v; break;
    case 0x3D4: crtc_idx = (u8)v; break;
    case 0x3D5: crtc_reg[crtc_idx & 31] = (u8)v; break;
    case 0x3F8: if (!(ser_lcr & 0x80)) serial_out((u8)v); break;
    case 0x3F9: if (!(ser_lcr & 0x80)) { ser_ier = (u8)v; if ((v & 1) && ser_t != ser_h) pic_raise(4); } break;
    case 0x3FB: ser_lcr = (u8)v; break;
    case 0x604: if ((v & 0xFFFF) == 0x2000) stop_reason = STOP_POWEROFF; break;
    case 0xB004: if ((v & 0xFFFF) == 0x2000) stop_reason = STOP_POWEROFF; break;
    default: break;
    }
}

/* ---- aritmetik/mantık --------------------------------------------------------------------- */
static u32 do_add(u32 a, u32 b, u32 c, int sz)
{
    u32 m = MASK(sz); a &= m; b &= m;
    u64 t = (u64)a + b + c; u32 r = (u32)t & m;
    set_flag(CF, t > m); set_flag(AF, ((a ^ b ^ r) & 0x10) != 0);
    set_flag(OF, (~(a ^ b) & (a ^ r) & SIGN(sz)) != 0);
    set_szp(r, sz);
    return r;
}
static u32 do_sub(u32 a, u32 b, u32 c, int sz)
{
    u32 m = MASK(sz); a &= m; b &= m;
    u32 r = (u32)((u64)a - b - c) & m;
    set_flag(CF, (u64)a < (u64)b + c); set_flag(AF, ((a ^ b ^ r) & 0x10) != 0);
    set_flag(OF, ((a ^ b) & (a ^ r) & SIGN(sz)) != 0);
    set_szp(r, sz);
    return r;
}
static u32 do_logic(int op, u32 a, u32 b, int sz)
{
    u32 r = (op == 1 ? (a | b) : op == 4 ? (a & b) : (a ^ b)) & MASK(sz);
    set_flag(CF, 0); set_flag(OF, 0); set_flag(AF, 0);
    set_szp(r, sz);
    return r;
}
static u32 alu(int op, u32 a, u32 b, int sz)
{
    switch (op) {
    case 0: return do_add(a, b, 0, sz);
    case 1: case 4: case 6: return do_logic(op, a, b, sz);
    case 2: return do_add(a, b, (C.eflags & CF) ? 1 : 0, sz);
    case 3: return do_sub(a, b, (C.eflags & CF) ? 1 : 0, sz);
    default: return do_sub(a, b, 0, sz);                 /* 5 = sub, 7 = cmp */
    }
}

static u32 do_shift(int op, u32 v, u32 cnt, int sz)
{
    cnt &= 31;
    if (!cnt) return v;
    u32 m = MASK(sz), sg = SIGN(sz); int bits = sz * 8;
    v &= m;
    u32 r = 0; int cf = 0;
    switch (op) {
    case 0: { u32 c = cnt % bits; r = c ? (((v << c) | (v >> (bits - c))) & m) : v; set_flag(CF, r & 1);
              set_flag(OF, ((r & sg) != 0) ^ (r & 1)); return r; }
    case 1: { u32 c = cnt % bits; r = c ? (((v >> c) | (v << (bits - c))) & m) : v; set_flag(CF, (r & sg) != 0);
              set_flag(OF, ((r & sg) != 0) ^ ((r & (sg >> 1)) != 0)); return r; }
    case 4: case 6: { u64 t = (u64)v << cnt; r = (u32)t & m; cf = (t >> bits) & 1;
                      set_flag(OF, ((r & sg) != 0) ^ cf); break; }
    case 5: { cf = (v >> (cnt - 1)) & 1; r = v >> cnt; set_flag(OF, (v & sg) != 0); break; }
    case 7: { s32 sv = sz == 4 ? (s32)v : sz == 2 ? (s16)v : (s8)v;
              cf = (sv >> (cnt - 1)) & 1; r = (u32)(sv >> cnt) & m; set_flag(OF, 0); break; }
    default: die("shift op %d desteklenmiyor", op);
    }
    set_flag(CF, cf); set_szp(r, sz);
    return r;
}

static int cc_eval(int cc)
{
    int r = 0;
    int cf = !!(C.eflags & CF), zf = !!(C.eflags & ZF), sf = !!(C.eflags & SF), of = !!(C.eflags & OF), pf = !!(C.eflags & PF);
    switch (cc >> 1) {
    case 0: r = of; break; case 1: r = cf; break; case 2: r = zf; break; case 3: r = cf | zf; break;
    case 4: r = sf; break; case 5: r = pf; break; case 6: r = sf != of; break; case 7: r = zf | (sf != of); break;
    }
    return (cc & 1) ? !r : r;
}

/* ---- ModRM ---------------------------------------------------------------------------------- */
static int g_opsz, g_adsz, g_seg_ov;
typedef struct { int mod, reg, rm, is_reg, seg; u32 off; } Modrm;

static u8 fetch8(void) { u8 v = (u8)mem_rd(C.s[CS].base + C.eip, 1); C.eip++; return v; }
static u32 fetch16(void) { u32 v = mem_rd(C.s[CS].base + C.eip, 2); C.eip += 2; return v; }
static u32 fetch32(void) { u32 v = mem_rd(C.s[CS].base + C.eip, 4); C.eip += 4; return v; }
static u32 fetch_imm(int sz) { return sz == 4 ? fetch32() : sz == 2 ? fetch16() : fetch8(); }

static void get_modrm(Modrm *m)
{
    u8 b = fetch8();
    m->mod = b >> 6; m->reg = (b >> 3) & 7; m->rm = b & 7; m->is_reg = 0;
    if (m->mod == 3) { m->is_reg = 1; return; }
    int seg = DS;
    u32 off = 0;
    if (g_adsz == 4) {
        if (m->rm == 4) {
            u8 sib = fetch8();
            int sc = sib >> 6, idx = (sib >> 3) & 7, base = sib & 7;
            if (base == 5 && m->mod == 0) off = fetch32();
            else { off = C.r[base]; if (base == 4 || base == 5) seg = SS; }
            if (idx != 4) off += C.r[idx] << sc;
        } else if (m->rm == 5 && m->mod == 0) {
            off = fetch32();
        } else {
            off = C.r[m->rm];
            if (m->rm == 5) seg = SS;
        }
        if (m->mod == 1) off += (u32)(s32)(s8)fetch8();
        else if (m->mod == 2) off += fetch32();
    } else {
        u32 bx = C.r[EBX] & 0xFFFF, bp = C.r[EBP] & 0xFFFF, si = C.r[ESI] & 0xFFFF, di = C.r[EDI] & 0xFFFF;
        switch (m->rm) {
        case 0: off = bx + si; break; case 1: off = bx + di; break;
        case 2: off = bp + si; seg = SS; break; case 3: off = bp + di; seg = SS; break;
        case 4: off = si; break; case 5: off = di; break;
        case 6: if (m->mod == 0) off = fetch16(); else { off = bp; seg = SS; } break;
        case 7: off = bx; break;
        }
        if (m->mod == 1) off += (u32)(s32)(s8)fetch8();
        else if (m->mod == 2) off += fetch16();
        off &= 0xFFFF;
    }
    m->seg = g_seg_ov >= 0 ? g_seg_ov : seg;
    m->off = off;
}

static u32 rm_rd(Modrm *m, int sz) { return m->is_reg ? rd_reg(m->rm, sz) : mem_rd(C.s[m->seg].base + m->off, sz); }
static void rm_wr(Modrm *m, int sz, u32 v) { if (m->is_reg) wr_reg(m->rm, sz, v); else mem_wr(C.s[m->seg].base + m->off, v, sz); }

/* ---- kesme/istisna teslimi ------------------------------------------------------------------ */
static u32 flags_mask_modify(void) { return CF | PF | AF | ZF | SF | TF | DF | OF | 0x4000u; }

static void deliver(int vec, int err, int has_err, int soft)
{
    if (!(C.cr0 & 1)) die("gercek modda kesme teslimi (vec %d)", vec);
    if ((u32)vec * 8 + 7 > C.idt_limit) raise_x(13, vec * 8 + 2, 1);
    u32 lo = sup_rd(C.idt_base + vec * 8, 4), hi = sup_rd(C.idt_base + vec * 8 + 4, 4);
    u8 type = (u8)(hi >> 8);
    u16 sel = (u16)(lo >> 16);
    u32 off = (lo & 0xFFFF) | (hi & 0xFFFF0000u);
    if (!(type & 0x80)) raise_x(11, vec * 8 + 2, 1);
    int gt = type & 0x1F, gdpl = (type >> 5) & 3;
    if (gt != 0x0E && gt != 0x0F) die("kapi tipi %02x (vec %d) desteklenmiyor", gt, vec);
    if (soft && C.cpl > gdpl) raise_x(13, vec * 8 + 2, 1);
    Desc d;
    if (read_desc(sel, &d) < 0 || (d.access & 0x18) != 0x18) raise_x(13, sel & 0xFFFC, 1);
    if (d.dpl > C.cpl) raise_x(13, sel & 0xFFFC, 1);
    int new_cpl = d.dpl;

    u32 old_eflags = C.eflags, old_cs = C.s[CS].sel, old_eip = C.eip, old_esp = C.r[ESP], old_ss = C.s[SS].sel;
    if (new_cpl < C.cpl) {
        u32 esp0 = sup_rd(C.tr_base + 4, 4), ss0 = sup_rd(C.tr_base + 8, 4) & 0xFFFF;
        C.cpl = new_cpl;
        load_seg(SS, (u16)ss0);
        C.r[ESP] = esp0;
        push(old_ss, 4);
        push(old_esp, 4);
    }
    push(old_eflags, 4);
    push(old_cs, 4);
    push(old_eip, 4);
    if (has_err) push((u32)err, 4);
    load_cs(sel, new_cpl);
    C.eip = off;
    if (gt == 0x0E) C.eflags &= ~IF;
    C.eflags &= ~(TF | 0x4000u);
}

static void do_iret(void)
{
    if (!(C.cr0 & 1)) die("gercek mod iret");
    u32 eip = pop(4), cs = pop(4) & 0xFFFF, efl = pop(4);
    int rpl = cs & 3;
    if (rpl < C.cpl) raise_x(13, cs & 0xFFFC, 1);
    u32 mask = flags_mask_modify();
    if (C.cpl <= (int)((C.eflags >> 12) & 3)) mask |= IF;
    if (C.cpl == 0) mask |= 0x3000u;
    if (rpl > C.cpl) {
        u32 esp = pop(4), ss = pop(4) & 0xFFFF;
        load_cs((u16)cs, rpl);
        load_seg(SS, (u16)ss);
        C.r[ESP] = esp;
        for (int sr = ES; sr <= GS; sr++) {                 /* ring 3'e dönerken ayrıcalıklı bölütleri boşalt */
            if (sr == CS || sr == SS) continue;
            if (C.s[sr].present && C.s[sr].dpl < rpl) { C.s[sr].present = 0; C.s[sr].sel = 0; }
        }
    } else {
        load_cs((u16)cs, rpl);
    }
    C.eip = eip;
    C.eflags = (C.eflags & ~mask) | (efl & mask) | 2;
}

/* ---- BIOS taklidi (gerçek mod INT) ------------------------------------------------------------ */
static void set_cf(int on) { set_flag(CF, on); }

static void bios_int(int n)
{
    u32 ax = C.r[EAX] & 0xFFFF, ah = (C.r[EAX] >> 8) & 0xFF;
    switch (n) {
    case 0x10:
        if (ah == 0x00) for (int i = 0; i < 80 * 25; i++) phys_wr(0xB8000 + i * 2, 0x0720, 2);
        break;                                            /* 0x0E teletype: yok sayılır */
    case 0x13:
        if (ah == 0x41) { C.r[EBX] = (C.r[EBX] & 0xFFFF0000u) | 0xAA55; C.r[ECX] = (C.r[ECX] & 0xFFFF0000u) | 1; set_cf(0); }
        else if (ah == 0x42) {
            u32 dap = C.s[DS].base + (C.r[ESI] & 0xFFFF);
            u32 count = phys_rd(dap + 2, 2), off = phys_rd(dap + 4, 2), seg = phys_rd(dap + 6, 2);
            u32 lba = phys_rd(dap + 8, 4);
            if ((u64)(lba + count) * 512 > disk_size) { C.r[EAX] = (C.r[EAX] & 0xFFFF00FFu) | 0x0400; set_cf(1); break; }
            memcpy(ram + (seg << 4) + off, disk + (u64)lba * 512, (size_t)count * 512);
            C.r[EAX] &= 0xFFFF00FFu; set_cf(0);
        } else if (ah == 0x00) { C.r[EAX] &= 0xFFFF00FFu; set_cf(0); }
        else die("INT 13h AH=%02x desteklenmiyor", ah);
        break;
    case 0x15:
        if (C.r[EAX] == 0xE820) {
            static const struct { u64 b, l; u32 t; } mp[] = {
                { 0, 0x9FC00, 1 }, { 0x9FC00, 0x400, 2 }, { 0xF0000, 0x10000, 2 },
                { 0x100000, 0, 1 }, { 0, 0x20000, 2 }, { 0xFFFC0000ull, 0x40000, 2 } };
            u32 idx = C.r[EBX];
            if (C.r[EDX] != 0x534D4150 || idx >= 6) { set_cf(1); break; }
            u64 b = mp[idx].b, l = mp[idx].l;
            if (idx == 3) l = ram_size - 0x100000 - 0x20000;
            if (idx == 4) b = ram_size - 0x20000;
            u32 da = C.s[ES].base + (C.r[EDI] & 0xFFFF);
            phys_wr(da, (u32)b, 4); phys_wr(da + 4, (u32)(b >> 32), 4);
            phys_wr(da + 8, (u32)l, 4); phys_wr(da + 12, (u32)(l >> 32), 4);
            phys_wr(da + 16, mp[idx].t, 4);
            C.r[EAX] = 0x534D4150; C.r[ECX] = 24; C.r[EBX] = (idx + 1 >= 6) ? 0 : idx + 1; set_cf(0);
        } else if (ax == 0x2401) { C.r[EAX] &= 0xFFFF00FFu; set_cf(0); }
        else { C.r[EAX] = (C.r[EAX] & 0xFFFF00FFu) | 0x8600; set_cf(1); }
        break;
    default: die("BIOS INT %02x desteklenmiyor", n);
    }
}

/* ---- ana yürütme -------------------------------------------------------------------------------- */
static void string_op(int op, int sz, int rep)
{
    int dir = (C.eflags & DF) ? -sz : sz;
    u32 cmask = g_adsz == 4 ? 0xFFFFFFFFu : 0xFFFFu;
    int srcseg = g_seg_ov >= 0 ? g_seg_ov : DS;
    for (;;) {
        if (rep && !(C.r[ECX] & cmask)) break;
        u32 si = C.r[ESI] & cmask, di = C.r[EDI] & cmask;
        if (op == 0)      mem_wr(C.s[ES].base + di, mem_rd(C.s[srcseg].base + si, sz), sz);
        else if (op == 1) mem_wr(C.s[ES].base + di, C.r[EAX] & MASK(sz), sz);
        else              wr_reg(EAX, sz, mem_rd(C.s[srcseg].base + si, sz));
        if (op != 1) C.r[ESI] = (C.r[ESI] & ~cmask) | ((si + dir) & cmask);
        if (op != 2) C.r[EDI] = (C.r[EDI] & ~cmask) | ((di + dir) & cmask);
        if (!rep) break;
        C.r[ECX] = (C.r[ECX] & ~cmask) | ((C.r[ECX] - 1) & cmask);
    }
}

static void port_check(void)
{
    if ((C.cr0 & 1) && C.cpl > (int)((C.eflags >> 12) & 3)) raise_x(13, 0, 1);
}
static void priv_check(void) { if ((C.cr0 & 1) && C.cpl != 0) raise_x(13, 0, 1); }

static void imul_rm(Modrm *m, int sz, u32 imm, int have_imm)
{
    s32 a = (sz == 4) ? (s32)rm_rd(m, 4) : (s16)rm_rd(m, 2);
    s32 b = have_imm ? (sz == 4 ? (s32)imm : (s16)imm) : (sz == 4 ? (s32)C.r[m->reg] : (s16)C.r[m->reg]);
    s64 res = (s64)a * b;
    u32 r = (u32)res & MASK(sz);
    int ovf = sz == 4 ? (res != (s64)(s32)r) : (res != (s64)(s16)r);
    wr_reg(m->reg, sz, r);
    set_flag(CF, ovf); set_flag(OF, ovf);
}

static void exec_one(void)
{
    memcpy(snap_r, C.r, sizeof snap_r); snap_eip = C.eip; snap_eflags = C.eflags;
    trace[trace_i++ & 31] = C.s[CS].base + C.eip;
    g_opsz = C.s[CS].d ? 4 : 2; g_adsz = g_opsz; g_seg_ov = -1;
    int rep = 0;
    u8 op;
    for (;;) {
        op = fetch8();
        switch (op) {
        case 0x66: g_opsz ^= 6; continue;
        case 0x67: g_adsz ^= 6; continue;
        case 0x26: g_seg_ov = ES; continue; case 0x2E: g_seg_ov = CS; continue;
        case 0x36: g_seg_ov = SS; continue; case 0x3E: g_seg_ov = DS; continue;
        case 0x64: g_seg_ov = FS; continue; case 0x65: g_seg_ov = GS; continue;
        case 0xF2: rep = 2; continue; case 0xF3: rep = 1; continue; case 0xF0: continue;
        }
        break;
    }
    int osz = g_opsz;
    Modrm m;

    if (op < 0x40 && (op & 7) < 6) {                           /* ALU: add or adc sbb and sub xor cmp */
        int a = op >> 3, form = op & 7;
        switch (form) {
        case 0: case 1: { int sz = form == 0 ? 1 : osz; get_modrm(&m);
            u32 r = alu(a, rm_rd(&m, sz), rd_reg(m.reg, sz), sz); if (a != 7) rm_wr(&m, sz, r); break; }
        case 2: case 3: { int sz = form == 2 ? 1 : osz; get_modrm(&m);
            u32 r = alu(a, rd_reg(m.reg, sz), rm_rd(&m, sz), sz); if (a != 7) wr_reg(m.reg, sz, r); break; }
        case 4: { u32 r = alu(a, C.r[EAX] & 0xFF, fetch8(), 1); if (a != 7) wr_reg(EAX, 1, r); break; }
        case 5: { u32 r = alu(a, rd_reg(EAX, osz), fetch_imm(osz), osz); if (a != 7) wr_reg(EAX, osz, r); break; }
        }
        return;
    }
    if (op >= 0x40 && op <= 0x4F) {                            /* inc/dec r */
        int r = op & 7; u32 cf = C.eflags & CF;
        u32 v = (op < 0x48) ? do_add(rd_reg(r, osz), 1, 0, osz) : do_sub(rd_reg(r, osz), 1, 0, osz);
        wr_reg(r, osz, v); C.eflags = (C.eflags & ~CF) | cf;
        return;
    }
    if (op >= 0x50 && op <= 0x57) { push(rd_reg(op & 7, osz), osz); return; }
    if (op >= 0x58 && op <= 0x5F) { u32 v = pop(osz); wr_reg(op & 7, osz, v); return; }
    if (op >= 0x70 && op <= 0x7F) { s8 rel = (s8)fetch8(); if (cc_eval(op & 15)) C.eip += rel; return; }
    if (op >= 0xB0 && op <= 0xB7) { wr_reg(op & 7, 1, fetch8()); return; }
    if (op >= 0xB8 && op <= 0xBF) { wr_reg(op & 7, osz, fetch_imm(osz)); return; }
    if (op >= 0x91 && op <= 0x97) { u32 t = rd_reg(EAX, osz); wr_reg(EAX, osz, rd_reg(op & 7, osz)); wr_reg(op & 7, osz, t); return; }

    switch (op) {
    case 0x06: push(C.s[ES].sel, osz); return; case 0x0E: push(C.s[CS].sel, osz); return;
    case 0x16: push(C.s[SS].sel, osz); return; case 0x1E: push(C.s[DS].sel, osz); return;
    case 0x07: { u32 v = pop(osz); load_seg(ES, (u16)v); return; }
    case 0x17: { u32 v = pop(osz); load_seg(SS, (u16)v); C.shadow = 1; return; }
    case 0x1F: { u32 v = pop(osz); load_seg(DS, (u16)v); return; }
    case 0x60: { u32 sp = C.r[ESP]; push(C.r[EAX], osz); push(C.r[ECX], osz); push(C.r[EDX], osz); push(C.r[EBX], osz);
                 push(sp, osz); push(C.r[EBP], osz); push(C.r[ESI], osz); push(C.r[EDI], osz); return; }
    case 0x61: { u32 edi = pop(osz), esi = pop(osz), ebp = pop(osz); (void)pop(osz);
                 u32 ebx = pop(osz), edx = pop(osz), ecx = pop(osz), eax = pop(osz);
                 wr_reg(EDI, osz, edi); wr_reg(ESI, osz, esi); wr_reg(EBP, osz, ebp);
                 wr_reg(EBX, osz, ebx); wr_reg(EDX, osz, edx); wr_reg(ECX, osz, ecx); wr_reg(EAX, osz, eax); return; }
    case 0x68: push(fetch_imm(osz), osz); return;
    case 0x6A: push((u32)(s32)(s8)fetch8() & MASK(osz), osz); return;
    case 0x69: case 0x6B: { get_modrm(&m); u32 imm = op == 0x69 ? fetch_imm(osz) : (u32)(s32)(s8)fetch8(); imul_rm(&m, osz, imm, 1); return; }
    case 0x80: case 0x81: case 0x83: {
        int sz = op == 0x80 ? 1 : osz; get_modrm(&m);
        u32 imm = op == 0x81 ? fetch_imm(osz) : (op == 0x83 ? (u32)(s32)(s8)fetch8() : fetch8());
        u32 r = alu(m.reg, rm_rd(&m, sz), imm, sz);
        if (m.reg != 7) rm_wr(&m, sz, r);
        return; }
    case 0x84: case 0x85: { int sz = op == 0x84 ? 1 : osz; get_modrm(&m); do_logic(4, rm_rd(&m, sz), rd_reg(m.reg, sz), sz); return; }
    case 0x86: case 0x87: { int sz = op == 0x86 ? 1 : osz; get_modrm(&m); u32 a = rm_rd(&m, sz), b = rd_reg(m.reg, sz);
                            rm_wr(&m, sz, b); wr_reg(m.reg, sz, a); return; }
    case 0x88: case 0x89: { int sz = op == 0x88 ? 1 : osz; get_modrm(&m); rm_wr(&m, sz, rd_reg(m.reg, sz)); return; }
    case 0x8A: case 0x8B: { int sz = op == 0x8A ? 1 : osz; get_modrm(&m); wr_reg(m.reg, sz, rm_rd(&m, sz)); return; }
    case 0x8C: { get_modrm(&m); if (m.reg > 5) raise_x(6, 0, 0); u32 v = C.s[m.reg].sel;
                 if (m.is_reg) wr_reg(m.rm, osz, v); else mem_wr(C.s[m.seg].base + m.off, v, 2); return; }
    case 0x8D: { get_modrm(&m); if (m.is_reg) raise_x(6, 0, 0); wr_reg(m.reg, osz, m.off); return; }
    case 0x8E: { get_modrm(&m); if (m.reg > 5 || m.reg == CS) raise_x(6, 0, 0);
                 load_seg(m.reg, (u16)rm_rd(&m, 2)); if (m.reg == SS) C.shadow = 1; return; }
    case 0x8F: { u32 v = pop(osz); get_modrm(&m); rm_wr(&m, osz, v); return; }
    case 0x90: return;
    case 0x98: if (osz == 4) C.r[EAX] = (u32)(s32)(s16)C.r[EAX]; else wr_reg(EAX, 2, (u32)(s32)(s8)C.r[EAX]); return;
    case 0x99: if (osz == 4) C.r[EDX] = (C.r[EAX] & 0x80000000u) ? 0xFFFFFFFFu : 0; else wr_reg(EDX, 2, (C.r[EAX] & 0x8000) ? 0xFFFF : 0); return;
    case 0x9C: push(C.eflags & ~0x30000u, osz); return;
    case 0x9D: { u32 v = pop(osz); u32 mk = flags_mask_modify();
                 if (!(C.cr0 & 1) || C.cpl <= (int)((C.eflags >> 12) & 3)) mk |= IF;
                 if (!(C.cr0 & 1) || C.cpl == 0) mk |= 0x3000u;
                 if (osz == 2) mk &= 0xFFFF;
                 C.eflags = (C.eflags & ~mk) | (v & mk) | 2; return; }
    case 0xA0: case 0xA1: case 0xA2: case 0xA3: {
        u32 off = g_adsz == 4 ? fetch32() : fetch16(); int sz = (op & 1) ? osz : 1;
        int sr = g_seg_ov >= 0 ? g_seg_ov : DS;
        if (op < 0xA2) wr_reg(EAX, sz, mem_rd(C.s[sr].base + off, sz)); else mem_wr(C.s[sr].base + off, rd_reg(EAX, sz), sz);
        return; }
    case 0xA4: case 0xA5: string_op(0, op == 0xA4 ? 1 : osz, rep); return;
    case 0xAA: case 0xAB: string_op(1, op == 0xAA ? 1 : osz, rep); return;
    case 0xAC: case 0xAD: string_op(2, op == 0xAC ? 1 : osz, rep); return;
    case 0xA8: do_logic(4, C.r[EAX] & 0xFF, fetch8(), 1); return;
    case 0xA9: do_logic(4, rd_reg(EAX, osz), fetch_imm(osz), osz); return;
    case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
        int sz = (op & 1) ? osz : 1; get_modrm(&m);
        u32 cnt = op < 0xD0 ? fetch8() : (op < 0xD2 ? 1 : (C.r[ECX] & 0xFF));
        rm_wr(&m, sz, do_shift(m.reg, rm_rd(&m, sz), cnt, sz)); return; }
    case 0xC2: { u32 n = fetch16(); C.eip = pop(osz); set_sp((C.r[ESP] + n) & (C.s[SS].d ? 0xFFFFFFFFu : 0xFFFFu), C.s[SS].d ? 0xFFFFFFFFu : 0xFFFFu); return; }
    case 0xC3: C.eip = pop(osz); return;
    case 0xC6: case 0xC7: { int sz = op == 0xC6 ? 1 : osz; get_modrm(&m); rm_wr(&m, sz, fetch_imm(sz)); return; }
    case 0xC9: { u32 mask = C.s[SS].d ? 0xFFFFFFFFu : 0xFFFFu; set_sp(C.r[EBP] & mask, mask); u32 v = pop(osz); wr_reg(EBP, osz, v); return; }
    case 0xCB: { u32 eip = pop(osz), cs = pop(osz) & 0xFFFF; load_cs((u16)cs, (C.cr0 & 1) ? (int)(cs & 3) : 0); C.eip = eip; return; }
    case 0xCC: deliver(3, 0, 0, 1); return;
    case 0xCD: { u8 n = fetch8(); if (!(C.cr0 & 1)) bios_int(n); else deliver(n, 0, 0, 1); return; }
    case 0xCF: do_iret(); return;
    case 0xE4: case 0xE5: case 0xE6: case 0xE7: case 0xEC: case 0xED: case 0xEE: case 0xEF: {
        port_check();
        int sz = (op & 1) ? osz : 1;
        u16 port = (op < 0xE8) ? fetch8() : (u16)C.r[EDX];
        if (op & 2) port_out(port, rd_reg(EAX, sz), sz); else wr_reg(EAX, sz, port_in(port, sz));
        return; }
    case 0xE8: { s32 rel = osz == 4 ? (s32)fetch32() : (s16)fetch16(); push(C.eip, osz); C.eip = (C.eip + rel) & (osz == 2 ? 0xFFFF : 0xFFFFFFFFu); return; }
    case 0xE9: { s32 rel = osz == 4 ? (s32)fetch32() : (s16)fetch16(); C.eip = (C.eip + rel) & (osz == 2 ? 0xFFFF : 0xFFFFFFFFu); return; }
    case 0xEA: { u32 off = fetch_imm(osz); u16 sel = (u16)fetch16(); load_cs(sel, (C.cr0 & 1) ? (sel & 3) : 0); C.eip = off; return; }
    case 0xEB: { s8 rel = (s8)fetch8(); C.eip += rel; return; }
    case 0xF4: priv_check(); C.halted = 1; return;
    case 0xF5: C.eflags ^= CF; return; case 0xF8: C.eflags &= ~CF; return; case 0xF9: C.eflags |= CF; return;
    case 0xF6: case 0xF7: {
        int sz = op == 0xF6 ? 1 : osz; get_modrm(&m);
        u32 v = rm_rd(&m, sz);
        switch (m.reg) {
        case 0: case 1: do_logic(4, v, fetch_imm(sz), sz); return;
        case 2: rm_wr(&m, sz, ~v & MASK(sz)); return;
        case 3: { u32 r = do_sub(0, v, 0, sz); set_flag(CF, (v & MASK(sz)) != 0); rm_wr(&m, sz, r); return; }
        case 4: case 5: {
            int sg = m.reg == 5; u64 res; int ovf;
            if (sz == 1) { res = sg ? (u64)(s64)((s8)C.r[EAX] * (s8)v) : (u64)((C.r[EAX] & 0xFF) * (v & 0xFF));
                           wr_reg(EAX, 2, (u32)res & 0xFFFF);
                           ovf = sg ? ((s16)res != (s8)res) : ((res >> 8) != 0); }
            else if (sz == 2) { res = sg ? (u64)(s64)((s16)C.r[EAX] * (s16)v) : (u64)((C.r[EAX] & 0xFFFF) * (v & 0xFFFF));
                                wr_reg(EAX, 2, (u32)res & 0xFFFF); wr_reg(EDX, 2, (u32)(res >> 16) & 0xFFFF);
                                ovf = sg ? ((s32)res != (s16)res) : ((res >> 16) != 0); }
            else { res = sg ? (u64)((s64)(s32)C.r[EAX] * (s64)(s32)v) : (u64)C.r[EAX] * v;
                   C.r[EAX] = (u32)res; C.r[EDX] = (u32)(res >> 32);
                   ovf = sg ? ((s64)res != (s64)(s32)res) : ((res >> 32) != 0); }
            set_flag(CF, ovf); set_flag(OF, ovf); return; }
        default: {                                          /* div / idiv */
            int sg = m.reg == 7;
            if (sz == 1) {
                if (!(v & 0xFF)) raise_x(0, 0, 0);
                if (sg) { s32 d = (s16)C.r[EAX], q = d / (s8)v, r = d % (s8)v; if (q < -128 || q > 127) raise_x(0, 0, 0);
                          wr_reg(EAX, 1, (u32)q); wr_reg(4, 1, (u32)r); }
                else { u32 d = C.r[EAX] & 0xFFFF, q = d / (v & 0xFF), r = d % (v & 0xFF); if (q > 0xFF) raise_x(0, 0, 0);
                       wr_reg(EAX, 1, q); wr_reg(4, 1, r); }
            } else if (sz == 2) {
                if (!(v & 0xFFFF)) raise_x(0, 0, 0);
                u32 dd = ((C.r[EDX] & 0xFFFF) << 16) | (C.r[EAX] & 0xFFFF);
                if (sg) { s32 d = (s32)dd, dv = (s16)v, q = d / dv, r = d % dv; if (q < -32768 || q > 32767) raise_x(0, 0, 0);
                          wr_reg(EAX, 2, (u32)q); wr_reg(EDX, 2, (u32)r); }
                else { u32 q = dd / (v & 0xFFFF), r = dd % (v & 0xFFFF); if (q > 0xFFFF) raise_x(0, 0, 0);
                       wr_reg(EAX, 2, q); wr_reg(EDX, 2, r); }
            } else {
                if (!v) raise_x(0, 0, 0);
                u64 dd = ((u64)C.r[EDX] << 32) | C.r[EAX];
                if (sg) { s64 d = (s64)dd, dv = (s32)v, q = d / dv, r = d % dv; if (q < INT32_MIN || q > INT32_MAX) raise_x(0, 0, 0);
                          C.r[EAX] = (u32)q; C.r[EDX] = (u32)r; }
                else { u64 q = dd / v, r = dd % v; if (q > 0xFFFFFFFFull) raise_x(0, 0, 0); C.r[EAX] = (u32)q; C.r[EDX] = (u32)r; }
            }
            return; }
        } }
    case 0xFA: port_check(); C.eflags &= ~IF; return;
    case 0xFB: port_check(); C.eflags |= IF; C.shadow = 1; return;
    case 0xFC: C.eflags &= ~DF; return; case 0xFD: C.eflags |= DF; return;
    case 0xFE: { get_modrm(&m); u32 cf = C.eflags & CF; u32 v = m.reg == 0 ? do_add(rm_rd(&m, 1), 1, 0, 1) : do_sub(rm_rd(&m, 1), 1, 0, 1);
                 rm_wr(&m, 1, v); C.eflags = (C.eflags & ~CF) | cf; return; }
    case 0xFF: {
        get_modrm(&m);
        switch (m.reg) {
        case 0: case 1: { u32 cf = C.eflags & CF; u32 v = m.reg == 0 ? do_add(rm_rd(&m, osz), 1, 0, osz) : do_sub(rm_rd(&m, osz), 1, 0, osz);
                          rm_wr(&m, osz, v); C.eflags = (C.eflags & ~CF) | cf; return; }
        case 2: { u32 t = rm_rd(&m, osz); push(C.eip, osz); C.eip = t; return; }
        case 4: C.eip = rm_rd(&m, osz); return;
        case 6: push(rm_rd(&m, osz), osz); return;
        default: die("FF /%d desteklenmiyor", m.reg);
        } }
    case 0x0F: break;
    default: die("desteklenmeyen opcode %02x", op);
    }

    /* ---- iki baytlık opcode'lar ---- */
    u8 op2 = fetch8();
    if (op2 >= 0x80 && op2 <= 0x8F) { s32 rel = osz == 4 ? (s32)fetch32() : (s16)fetch16(); if (cc_eval(op2 & 15)) C.eip += rel; return; }
    if (op2 >= 0x90 && op2 <= 0x9F) { get_modrm(&m); rm_wr(&m, 1, cc_eval(op2 & 15)); return; }
    if (op2 >= 0x40 && op2 <= 0x4F) { get_modrm(&m); u32 v = rm_rd(&m, osz); if (cc_eval(op2 & 15)) wr_reg(m.reg, osz, v); return; }
    switch (op2) {
    case 0x00: { get_modrm(&m); if (m.reg != 3) die("0F 00 /%d desteklenmiyor", m.reg); priv_check();
                 Desc d; u16 sel = (u16)rm_rd(&m, 2); if (read_desc(sel, &d) < 0) raise_x(13, sel & 0xFFFC, 1);
                 if ((d.access & 0x0F) != 0x9 && (d.access & 0x0F) != 0xB) raise_x(13, sel & 0xFFFC, 1);
                 C.tr_base = d.base; return; }
    case 0x01: { get_modrm(&m); priv_check(); if (m.is_reg && m.reg != 7) die("0F 01 reg");
                 if (m.reg == 7) return;                                    /* invlpg */
                 if (m.reg != 2 && m.reg != 3) die("0F 01 /%d desteklenmiyor", m.reg);
                 u32 a = C.s[m.seg].base + m.off; u32 lim = mem_rd(a, 2), base = mem_rd(a + 2, 4);
                 if (osz == 2) base &= 0xFFFFFF;
                 if (m.reg == 2) { C.gdt_base = base; C.gdt_limit = lim; } else { C.idt_base = base; C.idt_limit = lim; }
                 return; }
    case 0x0B: raise_x(6, 0, 0);
    case 0x1F: get_modrm(&m); return;
    case 0x20: { get_modrm(&m); priv_check(); u32 v = m.reg == 0 ? C.cr0 : m.reg == 2 ? C.cr2 : m.reg == 3 ? C.cr3 : m.reg == 4 ? C.cr4 : 0;
                 C.r[m.rm] = v; return; }
    case 0x22: { get_modrm(&m); priv_check(); u32 v = C.r[m.rm];
                 if (m.reg == 0) C.cr0 = v; else if (m.reg == 2) C.cr2 = v; else if (m.reg == 3) C.cr3 = v; else if (m.reg == 4) C.cr4 = v;
                 return; }
    case 0x31: C.r[EAX] = (u32)C.icount; C.r[EDX] = (u32)(C.icount >> 32); return;
    case 0xA0: push(C.s[FS].sel, osz); return; case 0xA8: push(C.s[GS].sel, osz); return;
    case 0xA1: { u32 v = pop(osz); load_seg(FS, (u16)v); return; }
    case 0xA9: { u32 v = pop(osz); load_seg(GS, (u16)v); return; }
    case 0xA2: {
        u32 leaf = C.r[EAX];
        static const char brand[] = "AnkaOS x86emu virtual CPU\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0";
        if (leaf == 0) { C.r[EAX] = 1; memcpy(&C.r[EBX], "Anka", 4); memcpy(&C.r[EDX], "EmuT", 4); memcpy(&C.r[ECX], "est!", 4); }
        else if (leaf == 1) { C.r[EAX] = 0x543; C.r[EBX] = 0; C.r[ECX] = 0; C.r[EDX] = 0x8039; }
        else if (leaf == 0x80000000u) { C.r[EAX] = 0x80000004u; C.r[EBX] = C.r[ECX] = C.r[EDX] = 0; }
        else if (leaf >= 0x80000002u && leaf <= 0x80000004u) { const char *b = brand + (leaf - 0x80000002u) * 16; memcpy(&C.r[EAX], b, 4); memcpy(&C.r[EBX], b + 4, 4); memcpy(&C.r[ECX], b + 8, 4); memcpy(&C.r[EDX], b + 12, 4); }
        else C.r[EAX] = C.r[EBX] = C.r[ECX] = C.r[EDX] = 0;
        return; }
    case 0xAF: { get_modrm(&m); imul_rm(&m, osz, 0, 0); return; }
    case 0xB6: case 0xB7: { get_modrm(&m); int ssz = op2 == 0xB6 ? 1 : 2; wr_reg(m.reg, osz, rm_rd(&m, ssz)); return; }
    case 0xBE: case 0xBF: { get_modrm(&m); int ssz = op2 == 0xBE ? 1 : 2; u32 v = rm_rd(&m, ssz);
                            v = ssz == 1 ? (u32)(s32)(s8)v : (u32)(s32)(s16)v; wr_reg(m.reg, osz, v); return; }
    default: die("desteklenmeyen opcode 0F %02x", op2);
    }
}

/* ---- çalıştırma döngüsü ------------------------------------------------------------------------------ */
static void restore_snapshot(void) { memcpy(C.r, snap_r, sizeof snap_r); C.eip = snap_eip; C.eflags = snap_eflags; }

static int idle_state;     /* 1: HLT'de ve uyandıracak zamanlayıcı yok */

static void run(u64 budget)
{
    u64 end = C.icount + budget;
    idle_state = 0;
    while (C.icount < end && !stop_reason) {
        if (setjmp(jb)) {
            g_super = 0;
            if (in_deliver) {
                if (in_deliver >= 2) { fflush(stdout); fprintf(stderr, "\n*** UCLU HATA (triple fault)\n"); dump_state(); stop_reason = STOP_FATAL; return; }
                in_deliver = 2;
                deliver(8, 0, 1, 0);
            } else {
                restore_snapshot();
                in_deliver = 1;
                deliver(x_vec, x_err, x_has, 0);
            }
            in_deliver = 0;
            continue;
        }
        if (pit_on && C.icount >= pit_next) { pic_raise(0); pit_next = C.icount + pit_period; }
        int shadow = C.shadow; C.shadow = 0;
        if ((C.eflags & IF) && !shadow && (C.cr0 & 1)) {
            int v = pic_ack();
            if (v >= 0) { C.halted = 0; in_deliver = 1; deliver(v, 0, 0, 0); in_deliver = 0; }
        }
        if (C.halted) {
            if ((C.eflags & IF) && pit_on) { if (C.icount < pit_next) C.icount = pit_next; continue; }
            idle_state = 1;
            return;
        }
        exec_one();
        C.icount++;
    }
}

/* ---- betik (script) / klavye ----------------------------------------------------------------------------- */
static const char kmap[58] = {
    0, 27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b', '\t',
    'q','w','e','r','t','y','u','i','o','p','[',']', '\n', 0,
    'a','s','d','f','g','h','j','k','l',';','\'','`', 0, '\\',
    'z','x','c','v','b','n','m',',','.','/', 0, '*', 0, ' ',
};
static const char ksh[58] = {
    0, 27, '!','@','#','$','%','^','&','*','(',')','_','+', '\b', '\t',
    'Q','W','E','R','T','Y','U','I','O','P','{','}', '\n', 0,
    'A','S','D','F','G','H','J','K','L',':','"','~', 0, '|',
    'Z','X','C','V','B','N','M','<','>','?', 0, '*', 0, ' ',
};

static void kbd_type_char(unsigned char c)
{
    if (c == 3) { kbd_push(0x1D); kbd_push(0x2E); kbd_push(0xAE); kbd_push(0x9D); return; }       /* Ctrl-C */
    if (c == 0x11) { kbd_push(0xE0); kbd_push(0x48); kbd_push(0xE0); kbd_push(0xC8); return; }       /* Yukari */
    if (c == 0x12) { kbd_push(0xE0); kbd_push(0x50); kbd_push(0xE0); kbd_push(0xD0); return; }       /* Asagi  */
    for (int sc = 1; sc < 58; sc++) {
        if (kmap[sc] == (char)c) { kbd_push((u8)sc); kbd_push((u8)(sc | 0x80)); return; }
        if (ksh[sc] == (char)c)  { kbd_push(0x2A); kbd_push((u8)sc); kbd_push((u8)(sc | 0x80)); kbd_push(0xAA); return; }
    }
    die("send: '%c' tusu bilinmiyor", c);
}

static void unescape(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '\\' && s[1]) {
            s++;
            if (*s == 'n') *o++ = '\n'; else if (*s == 'r') *o++ = '\r'; else if (*s == 't') *o++ = '\t'; else if (*s == 'b') *o++ = '\b';
            else if (*s == '\\') *o++ = '\\';
            else if (*s == 'x' && s[1] && s[2]) { char h[3] = { s[1], s[2], 0 }; *o++ = (char)strtol(h, NULL, 16); s += 2; }
            else *o++ = *s;
        } else *o++ = *s;
    }
    *o = 0;
}

static void dump_screen(void)
{
    printf("\n+--------------------------------- VGA 80x25 ---------------------------------+\n");
    for (int r = 0; r < 25; r++) {
        char line[81];
        int last = -1;
        for (int c = 0; c < 80; c++) {
            u8 ch = ram[0xB8000 + (r * 80 + c) * 2];
            line[c] = (ch >= 32 && ch < 127) ? (char)ch : ' ';
            if (line[c] != ' ') last = c;
        }
        line[last + 1] = 0;
        printf("|%s\n", line);
    }
    printf("+ cursor=%u ------------------------------------------------------------------+\n",
           (crtc_reg[14] << 8) | crtc_reg[15]);
}

static void boot(void)
{
    if (disk_size < 512 || disk[510] != 0x55 || disk[511] != 0xAA) die("gecersiz boot sektoru");
    memcpy(ram + 0x7C00, disk, 512);
    C.s[CS].base = 0; C.s[CS].sel = 0; C.eip = 0x7C00;
    for (int i = 0; i < 6; i++) { C.s[i].limit = 0xFFFF; C.s[i].present = 1; }
    C.r[EDX] = 0x80; C.eflags = 0x202; C.cpl = 0;
    for (int i = 0; i < 2; i++) pic[i].imr = 0xFF;
}

int main(int argc, char **argv)
{
    u64 max_instr = 3000000000ull;
    int ai = 1;
    while (ai + 1 < argc && argv[ai][0] == '-') {
        if (!strcmp(argv[ai], "-n")) max_instr = strtoull(argv[ai + 1], NULL, 10);
        else if (!strcmp(argv[ai], "-m")) ram_size = (u32)atoi(argv[ai + 1]) << 20;   /* RAM MB */
        else break;
        ai += 2;
    }
    if (ai + 1 >= argc) { fprintf(stderr, "kullanim: x86emu [-n max_komut] [-m ram_mb] os.img script.txt\n"); return 1; }
    FILE *f = fopen(argv[ai], "rb");
    if (!f) { perror("os.img"); return 1; }
    fseek(f, 0, SEEK_END); disk_size = (u32)ftell(f); fseek(f, 0, SEEK_SET);
    disk = malloc(disk_size);
    if (fread(disk, 1, disk_size, f) != disk_size) return 1;
    fclose(f);
    ram = calloc(ram_size, 1);
    boot();

    FILE *sf = fopen(argv[ai + 1], "r");
    if (!sf) { perror("script"); return 1; }
    char line[1024];
    size_t scan = 0;
    int failures = 0;
    while (stop_reason != STOP_FATAL && fgets(line, sizeof line, sf)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0] || line[0] == '#') continue;
        char *arg = strchr(line, ' ');
        if (arg) *arg++ = 0; else arg = line + strlen(line);
        size_t al = strlen(arg);
        if (al >= 2 && arg[0] == '"' && arg[al - 1] == '"') { arg[al - 1] = 0; arg++; }
        unescape(arg);
        if (!strcmp(line, "expect")) {
            u64 deadline = C.icount + max_instr;
            for (;;) {
                char *hit = serial_log ? strstr(serial_log + scan, arg) : NULL;
                if (hit) { scan = (size_t)(hit - serial_log) + strlen(arg); break; }
                if (stop_reason || C.icount >= deadline || idle_state) {
                    fflush(stdout);
                    fprintf(stderr, "\n*** EXPECT BASARISIZ: \"%s\" (stop=%d idle=%d icount=%llu)\n", arg, stop_reason, idle_state, (unsigned long long)C.icount);
                    failures++; stop_reason = STOP_FATAL; break;
                }
                run(200000);
            }
        } else if (!strcmp(line, "expectstop")) {
            int want = !strcmp(arg, "reset") ? STOP_RESET : STOP_POWEROFF;
            u64 deadline = C.icount + max_instr;
            while (!stop_reason && C.icount < deadline && !idle_state) run(200000);
            if (stop_reason != want) {
                fflush(stdout);
                fprintf(stderr, "\n*** EXPECTSTOP BASARISIZ: '%s' bekleniyordu (stop=%d)\n", arg, stop_reason);
                failures++; stop_reason = STOP_FATAL;
            } else printf("\n### makine '%s' istedi (beklenen)\n", arg);
        } else if (!strcmp(line, "send")) { for (char *p = arg; *p; p++) kbd_type_char((u8)*p); }
        else if (!strcmp(line, "serial")) { for (char *p = arg; *p; p++) ser_push((u8)*p); }
        else if (!strcmp(line, "wait")) { u64 t = (u64)atoi(arg) * 11931 / 10; run(t * 100 / 100); }
        else if (!strcmp(line, "screen")) dump_screen();
        else if (!strcmp(line, "quit")) break;
        else die("bilinmeyen betik komutu: %s", line);
    }
    run(20000);                                                  /* son çıktıyı boşalt */
    fflush(stdout);
    fprintf(stderr, "\n[emu] bitti: icount=%llu stop_reason=%d failures=%d\n", (unsigned long long)C.icount, stop_reason, failures);
    return failures ? 1 : 0;
}
