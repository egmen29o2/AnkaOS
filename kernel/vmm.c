/* AnkaOS - sayfalama (paging)
 * Çekirdek: [0, mem_top) birebir (identity) eşlenir, 0. sayfa eşlenmez (NULL koruması).
 * Her kullanıcı işleminin kendi sayfa dizini vardır; çekirdek PDE'leri paylaşılır,
 * kullanıcı alanı (USER_BASE..USER_LIMIT) özeldir ve U/S bitiyle işaretlenir. */
#include <kernel.h>

static u32 kernel_pd;
static u32 map_top;

void paging_init(void)
{
    map_top = pmm_mem_top();
    kernel_pd = pmm_alloc_zeroed();
    if (!kernel_pd) kpanic("paging: cannot allocate page directory");
    u32 *pd = (u32 *)kernel_pd;

    for (u32 i = 0; i < (map_top >> 22); i++) {
        u32 pt = pmm_alloc_zeroed();
        if (!pt) kpanic("paging: out of memory for page tables");
        u32 *tab = (u32 *)pt;
        for (u32 j = 0; j < 1024; j++) tab[j] = ((i << 22) | (j << 12)) | PTE_P | PTE_RW;
        pd[i] = pt | PTE_P | PTE_RW;
    }
    ((u32 *)(pd[0] & ~0xFFFu))[0] = 0;          /* sayfa 0: eşleme yok -> NULL erişimi page fault */

    write_cr3(kernel_pd);
    write_cr0(read_cr0() | 0x80010000u);         /* PG | WP */
}

u32 vmm_kernel_pd(void) { return kernel_pd; }
u32 vmm_current(void)   { return read_cr3() & ~0xFFFu; }
void vmm_switch(u32 pd) { write_cr3(pd); }

u32 vmm_create_space(void)
{
    u32 pd = pmm_alloc_zeroed();
    if (!pd) return 0;
    memcpy((void *)pd, (void *)kernel_pd, (map_top >> 22) * 4);   /* çekirdek eşlemesini paylaş */
    return pd;
}

int vmm_map(u32 pd, u32 vaddr, u32 paddr, u32 flags)
{
    u32 *dir = (u32 *)pd;
    u32 di = vaddr >> 22, ti = (vaddr >> 12) & 0x3FF;
    if (di < (map_top >> 22)) return -1;                          /* çekirdek bölgesine dokunma */
    if (!(dir[di] & PTE_P)) {
        u32 pt = pmm_alloc_zeroed();
        if (!pt) return -1;
        dir[di] = pt | PTE_P | PTE_RW;
    }
    dir[di] |= (flags & PTE_US);
    u32 *tab = (u32 *)(dir[di] & ~0xFFFu);
    tab[ti] = (paddr & ~0xFFFu) | (flags & 0xFFF) | PTE_P;
    if (vmm_current() == pd) invlpg(vaddr);
    return 0;
}

/* kullanıcı alanındaki tüm çerçeveleri, sayfa tablolarını ve dizini serbest bırak */
void vmm_destroy_space(u32 pd)
{
    u32 *dir = (u32 *)pd;
    for (u32 i = USER_BASE >> 22; i < 1024; i++) {
        if (!(dir[i] & PTE_P)) continue;
        u32 *tab = (u32 *)(dir[i] & ~0xFFFu);
        for (u32 j = 0; j < 1024; j++)
            if (tab[j] & PTE_P) pmm_free(tab[j] & ~0xFFFu);
        pmm_free((u32)tab);
    }
    pmm_free(pd);
}

/* [addr, addr+len) kullanıcı aralığı gerçekten eşlenmiş ve erişilebilir mi? */
int vmm_user_ok(u32 pd, u32 addr, u32 len, int write)
{
    if (len == 0) return 1;
    if (addr < USER_BASE || addr >= USER_LIMIT || len > USER_LIMIT - addr) return 0;
    u32 *dir = (u32 *)pd;
    for (u32 a = addr & ~0xFFFu; a < addr + len; a += PAGE_SIZE) {
        u32 pde = dir[a >> 22];
        if (!(pde & PTE_P)) return 0;
        u32 pte = ((u32 *)(pde & ~0xFFFu))[(a >> 12) & 0x3FF];
        if (!(pte & PTE_P) || !(pte & PTE_US)) return 0;
        if (write && !(pte & PTE_RW)) return 0;
    }
    return 1;
}
