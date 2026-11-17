/* AnkaOS - fiziksel bellek yöneticisi (bitmap, 4 KB çerçeveler)
 * E820 haritasındaki "kullanılabilir" bölgeler 1 MB üstünden itibaren yönetilir.
 * En fazla 256 MB yönetilir (çekirdek bu aralığı birebir eşler). */
#include <kernel.h>

#define MAX_PHYS   0x10000000u
#define MAX_FRAMES (MAX_PHYS / PAGE_SIZE)

static u32 bitmap[MAX_FRAMES / 32];            /* 1 = kullanımda */
static u32 total_frames, free_count, hint, top;

static inline int  f_test(u32 f) { return bitmap[f >> 5] & (1u << (f & 31)); }
static inline void f_set(u32 f)  { bitmap[f >> 5] |= 1u << (f & 31); }
static inline void f_clr(u32 f)  { bitmap[f >> 5] &= ~(1u << (f & 31)); }

void pmm_init(const struct bootinfo *bi)
{
    memset(bitmap, 0xFF, sizeof bitmap);
    total_frames = free_count = hint = top = 0;

    u32 count = bi->e820_count;
    if (count > 40) count = 40;

    struct e820_entry fallback = { 0x100000, 15u * 1024 * 1024, 1, 1 };   /* E820 yoksa 16 MB varsay */
    const struct e820_entry *map = bi->e820;
    if (count == 0) { map = &fallback; count = 1; }

    for (u32 i = 0; i < count; i++) {
        if (map[i].type != 1) continue;                       /* 1 = kullanılabilir RAM */
        u64 b = map[i].base;
        u64 en = b + map[i].length;
        if (b >= MAX_PHYS) continue;
        if (en > MAX_PHYS) en = MAX_PHYS;
        u32 s = (u32)b, e = (u32)en;
        if (s < 0x100000) s = 0x100000;                       /* düşük 1 MB'a dokunma */
        s = (s + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        e &= ~(PAGE_SIZE - 1);
        if (s >= e) continue;
        for (u32 f = s >> 12; f < (e >> 12); f++) {
            f_clr(f);
            free_count++;
            total_frames++;
        }
        if (e > top) top = e;
    }
    top = (top + 0x3FFFFF) & ~0x3FFFFFu;                      /* 4 MB'a yuvarla (eşleme için) */
    if (top < 0x400000) top = 0x400000;
    if (top > MAX_PHYS) top = MAX_PHYS;
}

u32 pmm_alloc(void)
{
    u32 fl = irq_save();
    u32 nwords = top >> 17;                                   /* top / 4096 / 32 */
    for (u32 n = 0; n < nwords; n++) {
        u32 w = (hint + n) % nwords;
        if (bitmap[w] == 0xFFFFFFFFu) continue;
        for (u32 b = 0; b < 32; b++) {
            if (!(bitmap[w] & (1u << b))) {
                u32 f = w * 32 + b;
                f_set(f);
                free_count--;
                hint = w;
                irq_restore(fl);
                return f << 12;
            }
        }
    }
    irq_restore(fl);
    return 0;
}

u32 pmm_alloc_zeroed(void)
{
    u32 p = pmm_alloc();
    if (p) memset((void *)p, 0, PAGE_SIZE);
    return p;
}

/* ardışık 'count' çerçeve (örn. çekirdek yığını/heap için) */
u32 pmm_alloc_contig(u32 count)
{
    u32 fl = irq_save();
    u32 nframes = top >> 12, run = 0;
    for (u32 f = 256; f < nframes; f++) {
        if (f_test(f)) { run = 0; continue; }
        if (++run == count) {
            u32 start = f - count + 1;
            for (u32 i = start; i <= f; i++) f_set(i);
            free_count -= count;
            irq_restore(fl);
            return start << 12;
        }
    }
    irq_restore(fl);
    return 0;
}

void pmm_free(u32 phys)
{
    u32 f = phys >> 12;
    u32 fl = irq_save();
    if (phys & (PAGE_SIZE - 1) || f < 256 || f >= (top >> 12) || !f_test(f))
        kpanic("pmm_free: invalid or double free of 0x%08x", phys);
    f_clr(f);
    free_count++;
    irq_restore(fl);
}

u32 pmm_total_frames(void) { return total_frames; }
u32 pmm_free_frames(void)  { return free_count; }
u32 pmm_mem_top(void)      { return top; }
