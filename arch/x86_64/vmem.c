/* vmem.c -- the physical frame pool and per-space page tables.

   One static pool of 4 KiB frames serves both page-table pages and user
   data pages. It is in kernel bss, identity-mapped low and supervisor-only,
   so the kernel reaches any frame through its physical address, but ring 3
   reaches a frame only where vmem_map has placed it under PML4[1].

   A page table entry is phys | flags, flags being present (1), writable (2)
   and user (4). The shared kernel map is PML4 entry 0, taken from the boot
   PML4; private user pages are mapped under entry 1, the 512 GiB window at
   USER_VBASE. */

#include "hal.h"

extern uint64_t boot_pml4[512];

static uint8_t pool[POOL_FRAMES][FRAME_SIZE] __attribute__((aligned(FRAME_SIZE)));
static uint64_t freelist;          /* phys of the first free frame, 0 if none */
static uint32_t watermark;         /* frames never yet handed out */

int frame_in_pool(uint64_t phys) {
    uint64_t base = (uint64_t)pool;
    return (phys & (FRAME_SIZE - 1)) == 0 && phys >= base &&
           phys < base + (uint64_t)POOL_FRAMES * FRAME_SIZE;
}

static void zero_frame(uint64_t phys) {
    uint64_t *p = (uint64_t *)phys;
    for (unsigned i = 0; i < FRAME_SIZE / 8; i++) p[i] = 0;
}

uint64_t frame_alloc(void) {
    uint64_t f = 0;
    if (freelist) {
        f = freelist;
        freelist = *(uint64_t *)f;      /* next link, stored in the free frame */
    } else if (watermark < POOL_FRAMES) {
        f = (uint64_t)pool[watermark++];
    }
    if (f) zero_frame(f);
    return f;
}

void frame_free(uint64_t phys) {
    if (!frame_in_pool(phys)) arch_panic("frame_free of a frame outside the pool");
    *(uint64_t *)phys = freelist;
    freelist = phys;
}

uint64_t vspace_new(void) {
    uint64_t pml4 = frame_alloc();
    if (!pml4) return 0;
    /* Share the kernel's low map (and the shared user region) through entry
       0; private user pages come later under entry 1. The rest is zero. */
    ((uint64_t *)pml4)[0] = boot_pml4[0];
    return pml4;
}

static inline uint64_t *pte_slot(uint64_t table, uint64_t vaddr, int level) {
    uint64_t shift = 12 + 9 * (uint64_t)level;   /* level 3 = PML4 ... 0 = PT */
    return &((uint64_t *)table)[(vaddr >> shift) & 511];
}

/* The next table down, created from the pool if missing. */
static uint64_t walk(uint64_t table, uint64_t vaddr, int level) {
    uint64_t *e = pte_slot(table, vaddr, level);
    if (!(*e & 1)) {
        uint64_t next = frame_alloc();
        if (!next) return 0;
        *e = next | 0x7;                 /* present, writable, user */
    }
    return *e & ~0xfffull;
}

int vmem_map(uint64_t pml4, uint64_t vaddr, uint64_t phys, int writable) {
    if (vaddr < USER_VBASE || vaddr >= USER_VTOP || (vaddr & (FRAME_SIZE - 1)) ||
        (phys & (FRAME_SIZE - 1)) || !frame_in_pool(phys))
        return 0;
    uint64_t pdpt = walk(pml4, vaddr, 3);
    if (!pdpt) return 0;
    uint64_t pd = walk(pdpt, vaddr, 2);
    if (!pd) return 0;
    uint64_t pt = walk(pd, vaddr, 1);
    if (!pt) return 0;
    uint64_t *e = pte_slot(pt, vaddr, 0);
    if (*e & 1) return 0;                /* refuse to remap a live page */
    *e = phys | 0x5 | (writable ? 0x2 : 0);
    return 1;
}

int vmem_unmap(uint64_t pml4, uint64_t vaddr) {
    if (vaddr < USER_VBASE || vaddr >= USER_VTOP || (vaddr & (FRAME_SIZE - 1))) return 0;
    uint64_t t = pml4;
    for (int level = 3; level > 0; level--) {
        uint64_t *e = pte_slot(t, vaddr, level);
        if (!(*e & 1)) return 0;
        t = *e & ~0xfffull;
    }
    uint64_t *e = pte_slot(t, vaddr, 0);
    if (!(*e & 1)) return 0;
    *e = 0;
    uint64_t cur;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cur));
    if (cur == pml4) __asm__ volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
    return 1;
}

/* Walks the four levels for each page the range touches. Large pages (1 GiB
   at the PDPT level, 2 MiB at the PD level, as the boot map uses) end the
   walk early and cover their whole span. Every level must be present and
   user: the kernel's own pages are mapped supervisor-only, so they fail. */
int user_readable(uint64_t pml4, uint64_t va, uint64_t len) {
    if (len == 0) return 1;
    uint64_t end = va + len;
    if (end < va || end > USER_VTOP) return 0;
    uint64_t p = va & ~(uint64_t)(FRAME_SIZE - 1);
    while (p < end) {
        uint64_t table = pml4, step = FRAME_SIZE;
        for (int level = 3; level >= 0; level--) {
            uint64_t e = ((uint64_t *)table)[(p >> (12 + 9 * level)) & 511];
            if ((e & 0x5) != 0x5) return 0;              /* present and user */
            if (level > 0 && level < 3 && (e & 0x80)) {  /* a large page */
                step = 1ull << (12 + 9 * level);
                break;
            }
            table = e & 0x000ffffffffff000ull;
        }
        p = (p & ~(step - 1)) + step;
    }
    return 1;
}

/* Frees the private tables under entry 1 (three levels), then the PML4. The
   user pages themselves belong to whoever mapped them and are freed
   separately. */
static void free_table(uint64_t table, int level) {
    if (level > 0) {
        uint64_t *t = (uint64_t *)table;
        for (int i = 0; i < 512; i++)
            if (t[i] & 1) free_table(t[i] & ~0xfffull, level - 1);
    }
    frame_free(table);
}

void vspace_free(uint64_t pml4) {
    uint64_t entry1 = ((uint64_t *)pml4)[1];
    if (entry1 & 1) free_table(entry1 & ~0xfffull, 2);   /* PDPT, PDs, PTs */
    frame_free(pml4);
}
