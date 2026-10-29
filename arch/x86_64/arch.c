/* arch.c -- x86_64 machine setup and the C half of trap handling.

   Setup: serial, GDT + TSS, IDT, page-table adjustments, PIC, PIT, and the
   syscall MSRs. Then the Goose kernel is initialized and the first thread
   is entered.

   Traps: entry.S has already saved the interrupted thread into its frame.
   trap_entry filters what the core never needs to see (faults in ring 0,
   spurious IRQs), hands everything else to k_trap, and points the next
   entry at the frame of the thread k_trap picked. */

#include "hal.h"
#include "kernel_api.h"

struct frame arch_frames[MAX_THREADS] __attribute__((aligned(16)));
uint8_t gs_arena[ARENA_BYTES] __attribute__((aligned(2 << 20)));

/* Read by syscall_entry (entry.S). */
uint64_t cur_frame_end;
uint64_t syscall_user_rsp;

extern uint64_t boot_pd[512];
extern uint64_t boot_pml4[512];
extern uint64_t isr_stub_table[48];

/* 0 means the boot space; arch_switch resolves it to boot_pml4. */
uint64_t thread_cr3[MAX_THREADS];
extern void syscall_entry(void);
extern void arch_enter(struct frame *f) __attribute__((noreturn));

/* The user program's entry and the root thread's stack (user/). */
extern void user_main(void);
extern char user_root_stack_top[];

/* ---- control registers and MSRs ---- */

static inline uint64_t read_cr2(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr2, %0" : "=r"(v));
    return v;
}
static inline uint64_t read_cr3(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}
static inline void write_cr3(uint64_t v) { __asm__ volatile("mov %0, %%cr3" : : "r"(v) : "memory"); }

static inline void wrmsr(uint32_t msr, uint64_t v) {
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

static inline void io_wait(void) { outb(0x80, 0); }

/* ---- panics ---- */

void arch_panic(const char *why) {
    __asm__ volatile("cli");
    serial_puts("\n[kernel panic] ");
    serial_puts(why);
    serial_puts("\n[kernel panic] system halted\n");
    for (;;) __asm__ volatile("hlt");
}

static void dump_frame(const struct frame *f) {
    serial_puts("  vector "); serial_dec(f->vector);
    serial_puts(" error "); serial_hex(f->error);
    serial_puts("\n  rip "); serial_hex(f->rip);
    serial_puts(" rsp "); serial_hex(f->rsp);
    serial_puts(" rflags "); serial_hex(f->rflags);
    serial_puts("\n  rax "); serial_hex(f->rax);
    serial_puts(" rdi "); serial_hex(f->rdi);
    serial_puts(" rsi "); serial_hex(f->rsi);
    serial_putc('\n');
}

static void kernel_fault(const struct frame *f) __attribute__((noreturn));
static void kernel_fault(const struct frame *f) {
    serial_puts("\n[kernel panic] exception in ring 0\n");
    dump_frame(f);
    if (f->vector == 14) {
        uint64_t a = read_cr2();
        serial_puts("  cr2 "); serial_hex(a); serial_putc('\n');
        if (a >= (uint64_t)gs_arena && a < (uint64_t)gs_arena + ARENA_BYTES)
            serial_puts("  (a Goose data stack outgrew its 64 KiB region)\n");
    }
    arch_panic("unrecoverable kernel fault");
}

/* ---- GDT and TSS ---- */

struct tss {
    uint32_t reserved0;
    uint64_t rsp[3];
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

static struct tss tss;
static uint8_t emergency_stack[8192] __attribute__((aligned(16)));

/* Layout fixed by sysret: STAR[63:48] = 0x10 makes sysret load SS = 0x18
   and CS = 0x20, so user data must come right before user code. */
static uint64_t gdt[7] = {
    0,
    0x00af9a000000ffff, /* 0x08 kernel code, 64-bit */
    0x00cf92000000ffff, /* 0x10 kernel data */
    0x00cff2000000ffff, /* 0x18 user data, DPL 3 */
    0x00affa000000ffff, /* 0x20 user code, 64-bit, DPL 3 */
    0, 0,               /* 0x28 TSS (16 bytes) */
};

static void gdt_init(void) {
    uint64_t base = (uint64_t)&tss, limit = sizeof(tss) - 1;
    tss.iomap_base = sizeof(tss); /* no I/O bitmap: ring 3 gets no ports */
    tss.rsp[0] = (uint64_t)(emergency_stack + sizeof(emergency_stack));
    tss.ist[1] = (uint64_t)(emergency_stack + sizeof(emergency_stack)); /* IST2 */
    gdt[5] = (limit & 0xffff) | ((base & 0xffffff) << 16) | (0x89ull << 40) |
             (((limit >> 16) & 0xf) << 48) | (((base >> 24) & 0xff) << 56);
    gdt[6] = base >> 32;

    struct __attribute__((packed)) { uint16_t limit; uint64_t base; } gdtr = {
        sizeof(gdt) - 1, (uint64_t)gdt
    };
    __asm__ volatile(
        "lgdt %0\n\t"
        "pushq $0x08\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n"
        "1:\n\t"
        "movw $0x10, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%ss\n\t"
        "ltr %w1\n\t"
        : : "m"(gdtr), "r"(0x28) : "rax", "memory");
}

/* ---- IDT ---- */

struct idt_entry {
    uint16_t off_lo, sel;
    uint8_t ist, type;
    uint16_t off_mid;
    uint32_t off_hi, zero;
} __attribute__((packed));

static struct idt_entry idt[256];

static void idt_init(void) {
    for (int v = 0; v < 48; v++) {
        uint64_t a = isr_stub_table[v];
        /* NMI, #DF and #MC can arrive while a thread frame is being written;
           they get a stack of their own (IST2). Everything else lands in
           the current thread's frame (IST1). */
        int ist = (v == 2 || v == 8 || v == 18) ? 2 : 1;
        idt[v] = (struct idt_entry){
            .off_lo = (uint16_t)a, .sel = KERNEL_CS, .ist = (uint8_t)ist,
            .type = 0x8e, /* present, DPL 0, interrupt gate: IF cleared */
            .off_mid = (uint16_t)(a >> 16), .off_hi = (uint32_t)(a >> 32), .zero = 0,
        };
    }
    struct __attribute__((packed)) { uint16_t limit; uint64_t base; } idtr = {
        sizeof(idt) - 1, (uint64_t)idt
    };
    __asm__ volatile("lidt %0" : : "m"(idtr));
}

/* ---- paging ---- */

static uint64_t arena_pt[512] __attribute__((aligned(4096)));

static void paging_init(void) {
    uint64_t lo = (uint64_t)__user_start, hi = (uint64_t)__user_end;
    if (((lo | hi) & ((2u << 20) - 1)) || hi > (1ull << 30))
        arch_panic("user region must be 2 MiB aligned and inside the first 1 GiB");
    /* Open the user region to ring 3. Everything else stays supervisor-only,
       including page 0, so a user null dereference faults. */
    for (uint64_t a = lo; a < hi; a += 2u << 20) boot_pd[a >> 21] |= 0x4;

    /* The data-stack arena: 4 KiB pages, a hole after every region. */
    uint64_t base = (uint64_t)gs_arena;
    for (int i = 0; i < 512; i++) {
        int guard = i % REGION_STRIDE_PAGES == REGION_PAGES ||
                    i >= ARENA_REGIONS * REGION_STRIDE_PAGES;
        arena_pt[i] = guard ? 0 : (base + (uint64_t)i * 4096) | 0x3;
    }
    boot_pd[base >> 21] = (uint64_t)arena_pt | 0x3;
    write_cr3(read_cr3());
}

/* ---- PIC, PIT, serial ---- */

static void pic_init(void) {
    outb(PIC1_CMD, 0x11); io_wait();
    outb(PIC2_CMD, 0x11); io_wait();
    outb(PIC1_DATA, IRQ_BASE); io_wait();
    outb(PIC2_DATA, IRQ_BASE + 8); io_wait();
    outb(PIC1_DATA, 4); io_wait();
    outb(PIC2_DATA, 2); io_wait();
    outb(PIC1_DATA, 1); io_wait();
    outb(PIC2_DATA, 1); io_wait();
    /* Only the timer and the cascade start unmasked; a user driver that
       binds an IRQ unmasks its line (SYS_IRQ_BIND). */
    outb(PIC1_DATA, (uint8_t)~((1u << 0) | (1u << 2)));
    outb(PIC2_DATA, 0xff);
}

static void pit_init(void) {
    uint32_t div = 1193182 / TICK_HZ;
    outb(0x43, 0x34); /* channel 0, lo/hi byte, rate generator */
    outb(0x40, (uint8_t)div);
    outb(0x40, (uint8_t)(div >> 8));
}

static void serial_init(void) {
    outb(COM1 + 1, 0x00); /* no UART interrupts */
    outb(COM1 + 3, 0x80); /* divisor latch */
    outb(COM1 + 0, 0x01); /* 115200 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03); /* 8N1 */
    outb(COM1 + 2, 0xc7); /* FIFOs on */
    outb(COM1 + 4, 0x0b);
}

static void syscall_init(void) {
    wrmsr(0xc0000081, (0x10ull << 48) | ((uint64_t)KERNEL_CS << 32)); /* STAR */
    wrmsr(0xc0000082, (uint64_t)syscall_entry);                          /* LSTAR */
    wrmsr(0xc0000084, 0x44700); /* SFMASK: IF, TF, DF, NT, AC cleared on entry */
}

/* ---- switching ---- */

/* The next kernel entry, by interrupt or by syscall, saves into t's frame.
   Also point IST1 and the syscall stack at that frame, and load t's address
   space if it differs from the one now active. */
static struct frame *arch_switch(uint32_t t) {
    struct frame *f = frame_of(t);
    uint64_t end = (uint64_t)(f + 1);
    tss.ist[0] = end;
    cur_frame_end = end;
    uint64_t want = thread_cr3[t & (MAX_THREADS - 1)];
    if (!want) want = (uint64_t)boot_pml4;
    if (want != read_cr3()) write_cr3(want);
    return f;
}

/* NMI, #DF and #MC arrive on IST2, the emergency stack, not in a thread's
   frame, and none of them is the running thread's doing. A double fault or
   a machine check cannot be recovered from. Nothing in this kernel raises
   an NMI on purpose (no watchdog), so one is reported and the interrupted
   code, ring 3 or ring 0 alike, resumes from the emergency stack. */
static void machine_fault(const struct frame *f) __attribute__((noreturn));
static void machine_fault(const struct frame *f) {
    serial_puts(f->vector == 8 ? "\n[kernel panic] double fault\n"
                               : "\n[kernel panic] machine check\n");
    dump_frame(f);
    arch_panic("unrecoverable CPU exception");
}

struct frame *trap_entry(struct frame *f) {
    uint64_t vec = f->vector;
    if (vec == 2) {
        serial_puts("[kernel] NMI received and ignored\n");
        return f;
    }
    if (vec == 8 || vec == 18) machine_fault(f);
    if (vec < IRQ_BASE && (f->cs & 3) == 0) kernel_fault(f);
    if (vec == IRQ_BASE + 7 || vec == IRQ_BASE + 15) {
        /* A spurious IRQ has no in-service bit and gets no EOI from its own
           PIC (the master still needs one for a spurious slave line). */
        uint16_t cmd = vec == IRQ_BASE + 7 ? PIC1_CMD : PIC2_CMD;
        outb(cmd, 0x0b);
        if (!(inb(cmd) & 0x80)) {
            if (vec == IRQ_BASE + 15) outb(PIC1_CMD, 0x20);
            return f;
        }
    }
    uint64_t addr = vec == 14 ? read_cr2() : 0;
    return arch_switch(k_trap(vec, f->error, addr));
}

void kmain(uint32_t magic, uint32_t info) {
    (void)info;
    serial_init();
    serial_puts("\n[boot] goose-microkernel, x86_64\n");
    if (magic != 0x2badb002) serial_puts("[boot] warning: not started by a multiboot loader\n");
    gdt_init();
    idt_init();
    paging_init();
    pic_init();
    pit_init();
    syscall_init();
    serial_puts("[boot] cpu tables, paging, pic, pit and syscall ready\n");

    goose_init(0, 0);
    uint32_t first = k_init((uint64_t)user_main, (uint64_t)user_root_stack_top,
                            (uint64_t)__user_start, (uint64_t)__user_end);
    arch_enter(arch_switch(first));
}
