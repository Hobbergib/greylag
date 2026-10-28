/* klibc.c -- the few C library functions the generated kernel C and the C
   compiler itself expect: block copies and fills (gcc emits calls to these
   for struct copies), and the calloc the Goose runtime header uses once at
   boot to hold the data-stack descriptors. There is no general heap. */

#include <stddef.h>
#include <stdint.h>

#include "hal.h"

void *memcpy(void *dst, const void *src, size_t n) {
    void *d = dst;
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(src), "+c"(n) : : "memory");
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = dst;
    const uint8_t *s = src;
    if (d <= s || d >= s + n) return memcpy(dst, src, n);
    /* Overlapping with dst above src: copy backwards. */
    d += n - 1;
    s += n - 1;
    __asm__ volatile("std\n\trep movsb\n\tcld" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    void *d = dst;
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *p = a, *q = b;
    for (size_t i = 0; i < n; i++)
        if (p[i] != q[i]) return p[i] < q[i] ? -1 : 1;
    return 0;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

/* A bump allocator over a small static pool. The runtime allocates one
   block at boot (gs_stack_block) and never frees it. */
static uint8_t boot_pool[4096] __attribute__((aligned(16)));
static size_t boot_used;

void *malloc(size_t n) {
    n = (n + 15) & ~(size_t)15;
    if (n > sizeof(boot_pool) - boot_used) arch_panic("boot allocator exhausted");
    void *p = boot_pool + boot_used;
    boot_used += n;
    return p;
}

void *calloc(size_t count, size_t size) {
    if (size && count > ((size_t)-1) / size) arch_panic("calloc overflow");
    uint8_t *p = malloc(count * size);
    memset(p, 0, count * size);
    return p;
}

void free(void *p) { (void)p; }
