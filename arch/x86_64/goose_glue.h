/* goose_glue.h -- included into the generated kernel C (goose --include).
   It sits after the generated types and before the extern prototypes, so
   its static inline definitions are what the kernel's `extern fn`s bind to,
   and every call from Goose into the hardware layer compiles to a few
   instructions in place.

   Two parts:
   1. The HAL functions kernel/hal.goose declares. Their types are the
      generated C spellings of the Goose types (gs_u32 for u32, ...), so
      they match the prototypes the compiler emits after them.
   2. A freestanding implementation of the Goose runtime ABI that the
      generated code calls (runtime.h's GS_API functions): data-stack
      regions from a guarded static arena, text output to the serial port,
      and the failure paths as kernel panics. There is no heap, no OS and no
      threads behind any of it. */

#include "hal.h"

/* ======================= HAL for kernel/hal.goose ======================= */

/* The syscall ABI's registers by slot (kernel/hal.goose): rax, rdi, rsi,
   rdx, r10, r8, r9; then rip and rsp, which the core may read but not
   write. */
static inline gs_u64 hal_reg(gs_u32 t, gs_u32 slot) {
    static const uint8_t word[9] = { F_RAX, F_RDI, F_RSI, F_RDX, F_R10, F_R8, F_R9, F_RIP, F_RSP };
    return slot < 9 ? ((const uint64_t *)frame_of(t))[word[slot]] : 0;
}

static inline void hal_set_reg(gs_u32 t, gs_u32 slot, gs_u64 v) {
    static const uint8_t word[7] = { F_RAX, F_RDI, F_RSI, F_RDX, F_R10, F_R8, F_R9 };
    if (slot < 7) ((uint64_t *)frame_of(t))[word[slot]] = v;
}

/* An IPC transfer: the four message registers, rsi, rdx, r10 and r8, from
   the sender's frame to the receiver's. The message never leaves registers
   and frames. */
static inline void hal_copy_msg(gs_u32 from, gs_u32 to) {
    const struct frame *s = frame_of(from);
    struct frame *d = frame_of(to);
    d->rsi = s->rsi;
    d->rdx = s->rdx;
    d->r10 = s->r10;
    d->r8 = s->r8;
}

static inline void hal_thread_init(gs_u32 t, gs_u64 entry, gs_u64 stack, gs_u64 arg) {
    thread_frame_init(t, entry, stack, arg, 0);
    thread_set_space(t, 0);     /* the boot space until bound to another */
}

/* ---- address spaces ---- */

static inline gs_u64 hal_frame_alloc(void) { return frame_alloc(); }
static inline void hal_frame_free(gs_u64 phys) { frame_free(phys); }
static inline gs_u64 hal_vspace_new(void) { return vspace_new(); }
static inline void hal_vspace_free(gs_u64 pml4) { vspace_free(pml4); }
static inline uint8_t hal_frame_in_pool(gs_u64 phys) { return (uint8_t)frame_in_pool(phys); }
static inline uint8_t hal_map(gs_u64 pml4, gs_u64 vaddr, gs_u64 phys, uint8_t writable) {
    return (uint8_t)vmem_map(pml4, vaddr, phys, writable);
}
static inline uint8_t hal_unmap(gs_u64 pml4, gs_u64 vaddr) { return (uint8_t)vmem_unmap(pml4, vaddr); }
/* Binds a thread to an address space: cr3 of the space's PML4, or 0 for the
   boot space. */
static inline void hal_set_space(gs_u32 t, gs_u64 cr3) { thread_set_space(t, cr3); }

static inline void hal_idle_init(gs_u32 t) {
    thread_frame_init(t, (uint64_t)idle_thread_entry, (uint64_t)idle_stack_top, 0, 1);
}

static inline uint8_t hal_user_range(gs_u64 addr, gs_u64 len) { return (uint8_t)user_range_ok(addr, len); }

static inline void hal_irq_mask(gs_u32 irq) { if (irq < NUM_IRQS) pic_mask(irq); }
static inline void hal_irq_unmask(gs_u32 irq) { if (irq < NUM_IRQS) pic_unmask(irq); }
static inline void hal_irq_eoi(gs_u32 irq) { if (irq < NUM_IRQS) pic_eoi(irq); }

/* 64 for 0, as std's clz. */
static inline gs_i64 hal_clz(gs_u64 x) { return x ? __builtin_clzll(x) : 64; }

/* Writes a user buffer to the console. A trap leaves the calling thread's
   CR3 loaded until the kernel exits (arch_switch), so the buffer is read in
   place, through the caller's own page tables, once those tables say every
   byte of it is mapped and user-accessible: its private pages or the shared
   user region, never kernel memory. */
static inline gs_u64 hal_debug_write(gs_u64 addr, gs_u64 len) {
    if (len > 4096 || !user_readable(cur_cr3(), addr, len)) return 0;
    serial_write((const char *)addr, len);
    return len;
}

/* ===================== the Goose runtime, freestanding ==================== */

static int64_t gmk_regions_used;

static void gmk_where(const char *file, int line) {
    serial_puts(" at ");
    serial_puts(file);
    serial_putc(':');
    serial_dec((uint64_t)line);
}

static void gmk_sdec(int64_t v) {
    if (v < 0) { serial_putc('-'); serial_dec(0 - (uint64_t)v); }
    else serial_dec((uint64_t)v);
}

void gs_rt_start(int argc, char **argv, uint64_t reserve, uint64_t gap, uint64_t budget,
                 int64_t mainregions, int64_t workerregions) {
    (void)argc; (void)argv; (void)gap; (void)budget; (void)workerregions;
    if (reserve > REGION_BYTES)
        arch_panic("goose runtime: the kernel was compiled for data stacks larger than "
                   "its 64 KiB regions (build with --stack-reserve 64K)");
    if (mainregions > ARENA_REGIONS)
        arch_panic("goose runtime: the kernel needs more data stacks than the arena holds");
}

/* Regions are handed out once, at boot, and never returned: the kernel is
   one program instance that lives forever. */
uint8_t *gs_reserve_region(gs_stack *stack) {
    (void)stack;
    if (gmk_regions_used >= ARENA_REGIONS) arch_panic("goose runtime: out of data-stack regions");
    return gs_arena + (uint64_t)gmk_regions_used++ * REGION_STRIDE_PAGES * 4096;
}

void gs_out_bytes(const uint8_t *p, int64_t len) {
    if (len > 0) serial_write((const char *)p, (uint64_t)len);
}

int64_t gs_fmt_u64(uint8_t *dst, uint64_t v) {
    uint8_t buf[20];
    int64_t n = 0, k = 0;
    do { buf[n++] = (uint8_t)('0' + v % 10); v /= 10; } while (v);
    while (n) dst[k++] = buf[--n];
    return k;
}

int64_t gs_fmt_i64(uint8_t *dst, int64_t v) {
    if (v >= 0) return gs_fmt_u64(dst, (uint64_t)v);
    dst[0] = '-';
    return 1 + gs_fmt_u64(dst + 1, 0 - (uint64_t)v);
}

int64_t gs_fmt_bool(uint8_t *dst, int64_t v) {
    const char *s = v ? "true" : "false";
    int64_t n = 0;
    while (s[n]) { dst[n] = (uint8_t)s[n]; n++; }
    return n;
}

/* Byte arrays nested in an aggregate print quoted; the kernel only ever
   prints scalars and strings, so escapes are left out. */
int64_t gs_fmt_quoted(uint8_t *dst, const uint8_t *s, int64_t n) {
    int64_t k = 0;
    if (n > GS_FMT_MAX - 2) n = GS_FMT_MAX - 2;
    dst[k++] = '"';
    for (int64_t i = 0; i < n; i++) dst[k++] = s[i];
    dst[k++] = '"';
    return k;
}

/* No float support: the kernel is built with -mgeneral-regs-only, so
   gs_fmt_f64, gs_fmt_f32 and gs_asfail_f are deliberately not defined. A
   kernel that formats a float fails to link instead of corrupting the user
   threads' SSE registers. */

void gs_panic(const char *msg) {
    serial_puts("\n[kernel panic] goose runtime: ");
    arch_panic(msg);
}

void gs_abort(int err, const char *file, int line) {
    static const char *const why[] = {
        "limited array capacity exceeded", "slice bounds out of range",
        "relative reference offset overflow", "assert failed", "invalid capacity",
        "resize growth requires a fill value", "resize to a negative length",
        "pop on empty array", "unknown thread id", "a worker cannot wait for itself",
        "corrupt ADT tag", "serialization on a big-endian host", "bad slice pool length",
        "slice is not one of the pool's runs", "null self-relative target",
        "bad pop_n count", "bad queue limit",
    };
    serial_puts("\n[kernel panic] ");
    serial_puts(err >= 0 && err < (int)(sizeof(why) / sizeof(why[0])) ? why[err] : "abort");
    gmk_where(file, line);
    arch_panic("goose check failed");
}

void gs_abort_msg(const uint8_t *msg, int64_t len, const char *file, int line) {
    serial_puts("\n[kernel panic] ");
    if (len > 0) serial_write((const char *)msg, (uint64_t)len);
    gmk_where(file, line);
    arch_panic("abort");
}

void gs_exit(int64_t code) {
    serial_puts("\n[kernel] exit(");
    gmk_sdec(code);
    serial_putc(')');
    arch_panic("the kernel called exit");
}

void gs_idxfail(int64_t i, int64_t n, const char *file, int line) {
    serial_puts("\n[kernel panic] index ");
    gmk_sdec(i);
    serial_puts(" out of range for length ");
    gmk_sdec(n);
    gmk_where(file, line);
    arch_panic("bounds check failed");
}

void gs_divfail(const char *file, int line) {
    serial_puts("\n[kernel panic] division by zero");
    gmk_where(file, line);
    arch_panic("arithmetic check failed");
}

void gs_divovf(const char *file, int line) {
    serial_puts("\n[kernel panic] division overflow");
    gmk_where(file, line);
    arch_panic("arithmetic check failed");
}

void gs_ovf(int64_t a, const char *op, int64_t b, const char *type, const char *file, int line) {
    serial_puts("\n[kernel panic] overflow: ");
    gmk_sdec(a); serial_putc(' '); serial_puts(op); serial_putc(' '); gmk_sdec(b);
    serial_puts(" in "); serial_puts(type);
    gmk_where(file, line);
    arch_panic("arithmetic check failed");
}

void gs_ovf_neg(int64_t a, const char *type, const char *file, int line) {
    serial_puts("\n[kernel panic] overflow negating ");
    gmk_sdec(a); serial_puts(" in "); serial_puts(type);
    gmk_where(file, line);
    arch_panic("arithmetic check failed");
}

void gs_asfail_i(const char *why, int64_t v, const char *type, const char *file, int line) {
    serial_puts("\n[kernel panic] ");
    serial_puts(why); serial_putc(' '); gmk_sdec(v); serial_puts(" as "); serial_puts(type);
    gmk_where(file, line);
    arch_panic("conversion check failed");
}

void gs_asfail_u(const char *why, uint64_t v, const char *type, const char *file, int line) {
    serial_puts("\n[kernel panic] ");
    serial_puts(why); serial_putc(' '); serial_dec(v); serial_puts(" as "); serial_puts(type);
    gmk_where(file, line);
    arch_panic("conversion check failed");
}
