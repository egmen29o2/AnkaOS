/* AnkaOS - çekirdek heap'i: adres sıralı çift bağlı liste, ilk uygun (first-fit),
 * bölme ve birleştirme (coalescing). Kesmeler kapalıyken çalışır (yarış yok). */
#include <kernel.h>

struct block {
    u32 size;                 /* yalnızca yük (payload) boyutu */
    u32 used;
    struct block *next, *prev;
};
#define HDR       ((u32)sizeof(struct block))     /* 16 bayt */
#define MIN_SPLIT (HDR + 16)

static struct block *head;
static u32 heap_start, heap_size, used_bytes;

void heap_init(u32 start, u32 size)
{
    head = (struct block *)start;
    head->size = size - HDR;
    head->used = 0;
    head->next = head->prev = NULL;
    heap_start = start;
    heap_size = size;
    used_bytes = 0;
}

void *kmalloc(u32 size)
{
    if (!size) size = 1;
    size = (size + 15) & ~15u;
    u32 fl = irq_save();
    for (struct block *b = head; b; b = b->next) {
        if (b->used || b->size < size) continue;
        if (b->size >= size + MIN_SPLIT) {
            struct block *n = (struct block *)((u8 *)b + HDR + size);
            n->size = b->size - size - HDR;
            n->used = 0;
            n->prev = b;
            n->next = b->next;
            if (n->next) n->next->prev = n;
            b->next = n;
            b->size = size;
        }
        b->used = 1;
        used_bytes += b->size;
        irq_restore(fl);
        return (u8 *)b + HDR;
    }
    irq_restore(fl);
    return NULL;
}

void kfree(void *p)
{
    if (!p) return;
    u32 fl = irq_save();
    struct block *b = (struct block *)((u8 *)p - HDR);
    if ((u32)b < heap_start || (u32)b >= heap_start + heap_size || !b->used)
        kpanic("kfree: invalid pointer %p", p);
    b->used = 0;
    used_bytes -= b->size;
    if (b->next && !b->next->used) {                    /* sonrakiyle birleştir */
        b->size += HDR + b->next->size;
        b->next = b->next->next;
        if (b->next) b->next->prev = b;
    }
    if (b->prev && !b->prev->used) {                    /* öncekiyle birleştir */
        b->prev->size += HDR + b->size;
        b->prev->next = b->next;
        if (b->next) b->next->prev = b->prev;
    }
    irq_restore(fl);
}

void *krealloc(void *p, u32 size)
{
    if (!p) return kmalloc(size);
    struct block *b = (struct block *)((u8 *)p - HDR);
    if (size <= b->size) return p;
    void *n = kmalloc(size);
    if (!n) return NULL;
    memcpy(n, p, b->size);
    kfree(p);
    return n;
}

void heap_stats(u32 *used, u32 *free_bytes, u32 *blocks)
{
    u32 u = 0, f = 0, n = 0;
    u32 fl = irq_save();
    for (struct block *b = head; b; b = b->next) {
        n++;
        if (b->used) u += b->size; else f += b->size;
    }
    irq_restore(fl);
    if (used) *used = u;
    if (free_bytes) *free_bytes = f;
    if (blocks) *blocks = n;
}
