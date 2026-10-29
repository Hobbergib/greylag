/* hal.h -- the x86_64 hardware layer shared by the arch C code (arch.c) and
   the Goose kernel's glue (goose_glue.h, compiled into the generated
   kernel C). Everything that touches a register, a port or a page table
   lives on this side; every scheduling and IPC decision lives in Goose.

   The one data structure both sides agree on is the per-thread register
   frame. Trap entry pushes the interrupted thread's registers straight into
   its own frame (the IST stack pointer is the end of the current thread's
   frame), so saving and restoring a context never copies anything. */

#ifndef GMK_HAL_H
#define GMK_HAL_H

/* Must match kernel/config.goose. A power of two: the C side masks every
   thread id it is handed, so a bad id can never index past the frames. */
#define MAX_THREADS 64

#define KERNEL_CS 0x08
#define KERNEL_DS 0x10
#define USER_DS   0x1b /* GDT entry 3, RPL 3 */
#define USER_CS   0x23 /* GDT entry 4, RPL 3 */

/* Vector numbers as the kernel sees them. Hardware vectors are 0..255; a
   syscall arrives as 0x100 so it can never be confused with one. A frame
   whose vector is VEC_SYSCALL is resumed with sysretq, any other with
   iretq. */
#define VEC_SYSCALL 0x100
#define IRQ_BASE    32
#define NUM_IRQS    16

#define TICK_HZ 100

/* The Goose data stacks: one 2 MiB arena of 64 KiB regions, each followed by
   an unmapped 4 KiB guard page, so a stack that outgrows its region faults
   instead of running into the next one. REGION_BYTES is what the kernel is
   compiled with (goose --stack-reserve 64K): the compiler leaves out the
   growth checks a region of that size makes unnecessary. */
#define ARENA_BYTES         (2u << 20)
#define REGION_PAGES        16
#define REGION_BYTES        (REGION_PAGES * 4096u)
#define REGION_STRIDE_PAGES (REGION_PAGES + 1)
#define ARENA_REGIONS       (512 / REGION_STRIDE_PAGES)

/* Address spaces. Every space shares the kernel's low identity map through
   PML4 entry 0 (kernel code, data, stacks, the thread frames and the Goose
   arena, all supervisor-only, plus the shared user region, user-readable).
   A space's private user pages live under PML4 entry 1, the 512 GiB window
   at USER_VBASE, mapped from the frame pool. So kernel memory is reachable
   from every space (a trap keeps the faulting space's CR3), while private
   pages are per space. */
#define USER_VBASE   0x8000000000ull   /* PML4[1]: [512 GiB, 1 TiB) */
#define USER_VTOP    0x10000000000ull
#define FRAME_SIZE   4096u
#define POOL_FRAMES  512u              /* 2 MiB: page tables and user pages */

#ifndef __ASSEMBLER__

#include <stdint.h>

/* Lowest address first: the order entry.S pops in. 22 words, 176 bytes, a
   multiple of 16 so every frame's end is a valid (aligned) IST pointer. */
struct frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error;
    uint64_t rip, cs, rflags, rsp, ss;
};
_Static_assert(sizeof(struct frame) == 176, "frame layout must match entry.S");

/* Word index of each register inside struct frame. */
enum {
    F_R15, F_R14, F_R13, F_R12, F_R11, F_R10, F_R9, F_R8,
    F_RBP, F_RDI, F_RSI, F_RDX, F_RCX, F_RBX, F_RAX,
    F_VECTOR, F_ERROR, F_RIP, F_CS, F_RFLAGS, F_RSP, F_SS,
};

extern struct frame arch_frames[MAX_THREADS];
extern char __user_start[], __user_end[];
extern uint8_t gs_arena[ARENA_BYTES];

static inline struct frame *frame_of(uint32_t t) {
    return &arch_frames[t & (MAX_THREADS - 1)];
}

/* ---- port I/O ---- */

static inline void outb(uint16_t port, uint8_t v) {
    __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

/* ---- COM1, polled ---- */

#define COM1 0x3f8

static inline void serial_putc(char c) {
    if (c == '\n') serial_putc('\r');
    while (!(inb(COM1 + 5) & 0x20)) {}
    outb(COM1, (uint8_t)c);
}
static inline void serial_write(const char *s, uint64_t n) {
    for (uint64_t i = 0; i < n; i++) serial_putc(s[i]);
}
static inline void serial_puts(const char *s) {
    while (*s) serial_putc(*s++);
}
static inline void serial_hex(uint64_t v) {
    serial_puts("0x");
    for (int i = 60; i >= 0; i -= 4) serial_putc("0123456789abcdef"[(v >> i) & 15]);
}
static inline void serial_dec(uint64_t v) {
    char buf[20];
    int n = 0;
    do { buf[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) serial_putc(buf[--n]);
}

/* ---- 8259 PIC ---- */

#define PIC1_CMD 0x20
#define PIC1_DATA 0x21
#define PIC2_CMD 0xa0
#define PIC2_DATA 0xa1

static inline void pic_mask(uint32_t irq) {
    uint16_t port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    outb(port, inb(port) | (uint8_t)(1u << (irq & 7)));
}
static inline void pic_unmask(uint32_t irq) {
    uint16_t port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    outb(port, inb(port) & (uint8_t)~(1u << (irq & 7)));
}
static inline void pic_eoi(uint32_t irq) {
    if (irq >= 8) outb(PIC2_CMD, 0x20);
    outb(PIC1_CMD, 0x20);
}

/* ---- the user region ---- */

/* Every user thread runs in one shared address space (see the README): the
   user region is the 2 MiB-aligned span the linker script gathers user.o
   into, and the only memory with the U bit set in the page tables. */
static inline int user_range_ok(uint64_t addr, uint64_t len) {
    uint64_t lo = (uint64_t)__user_start, hi = (uint64_t)__user_end;
    return addr >= lo && addr <= hi && len <= hi - addr;
}

/* ---- context setup ---- */

static inline void thread_frame_init(uint32_t t, uint64_t entry, uint64_t stack, uint64_t arg,
                                     int kernel) {
    struct frame *f = frame_of(t);
    uint64_t *w = (uint64_t *)f;
    for (unsigned i = 0; i < sizeof(*f) / 8; i++) w[i] = 0;
    f->rip = entry;
    f->rsp = stack;
    f->rdi = arg;
    f->rflags = 0x202; /* IF, and the reserved bit 1 */
    f->cs = kernel ? KERNEL_CS : USER_CS;
    f->ss = kernel ? KERNEL_DS : USER_DS;
    f->vector = 0; /* not VEC_SYSCALL: the first entry goes through iretq */
}

/* arch.c */
void arch_panic(const char *why) __attribute__((noreturn));
void idle_thread_entry(void);
extern char idle_stack_top[];

/* The CR3 each thread runs under: 0 means the boot space (the shared kernel
   map alone). arch_switch loads it, switching only when it changes. */
extern uint64_t thread_cr3[MAX_THREADS];
static inline void thread_set_space(uint32_t t, uint64_t cr3) { thread_cr3[t & (MAX_THREADS - 1)] = cr3; }

/* vmem.c -- the physical frame pool and per-space page tables. Addresses
   are physical; the whole pool is identity-mapped low, so the kernel reads
   and writes a frame through its physical address. */
uint64_t frame_alloc(void);               /* a zeroed 4 KiB frame; 0 if none */
void frame_free(uint64_t phys);
uint64_t vspace_new(void);                /* a PML4 sharing the kernel map; 0 if none */
void vspace_free(uint64_t pml4);          /* the PML4 and its private tables, not the pages */
int vmem_map(uint64_t pml4, uint64_t vaddr, uint64_t phys, int writable);
int vmem_unmap(uint64_t pml4, uint64_t vaddr);
int frame_in_pool(uint64_t phys);         /* whether phys is a pool frame, page-aligned */
/* Whether every byte of [va, va + len) is mapped present and user-accessible
   under pml4: the check before the kernel reads a user buffer. */
int user_readable(uint64_t pml4, uint64_t va, uint64_t len);
static inline uint64_t cur_cr3(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}
static inline uint64_t peek(uint64_t phys) { return *(volatile uint64_t *)phys; }

#endif /* !__ASSEMBLER__ */
#endif
