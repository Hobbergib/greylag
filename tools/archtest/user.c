/* user.c (archtest) -- ring-3 checks of the arch layer, run under the C
   stand-in core. Each check exercises one path through entry.S:

   - callee-saved registers across 200 syscalls (the sysretq path);
   - all of them across timer preemption, while spinning with no syscall at
     all (the iretq path, and proof that the timer preempts ring 3);
   - a user read of kernel memory and a privileged instruction, which must
     fault and kill only their own thread;
   - a console write from a kernel address, which the HAL must refuse. */

#include "syscall.h"

#define STACK_BYTES 8192

static uint8_t stacks[6][STACK_BYTES] __attribute__((aligned(16)));
static uint8_t root_stack[STACK_BYTES] __attribute__((aligned(16), used));
__asm__(".globl user_root_stack_top\n\t.set user_root_stack_top, root_stack + 8184");

static volatile uint64_t flag, spins_done, faulters_ran;
static uint64_t failures;

static uint64_t len(const char *s) {
    uint64_t n = 0;
    while (s[n]) n++;
    return n;
}
static void say(const char *s) { sys_debug_write(s, len(s)); }

static void check(int ok, const char *what) {
    say(ok ? "[archtest] ok: " : "[archtest] FAIL: ");
    say(what);
    say("\n");
    if (!ok) failures++;
}

static void *stack_top(int i) { return stacks[i] + STACK_BYTES - 8; }

#define VERIFY_REGS                                                  \
    "movq %%rbx, %%rax\n\t xorq $0x1111, %%rax\n\t"                  \
    "movq %%r12, %%rcx\n\t xorq $0x2222, %%rcx\n\t orq %%rcx, %%rax\n\t" \
    "movq %%r13, %%rcx\n\t xorq $0x3333, %%rcx\n\t orq %%rcx, %%rax\n\t" \
    "movq %%r14, %%rcx\n\t xorq $0x4444, %%rcx\n\t orq %%rcx, %%rax\n\t" \
    "movq %%r15, %%rcx\n\t xorq $0x5555, %%rcx\n\t orq %%rcx, %%rax\n\t"

#define SET_REGS                  \
    "movq $0x1111, %%rbx\n\t"     \
    "movq $0x2222, %%r12\n\t"     \
    "movq $0x3333, %%r13\n\t"     \
    "movq $0x4444, %%r14\n\t"     \
    "movq $0x5555, %%r15\n\t"

static uint64_t yield_keeping_regs(void) {
    uint64_t bad;
    __asm__ volatile(
        SET_REGS
        "movq $200, %%r9\n"
        "1:\n\t"
        "movq $1, %%rax\n\t"     /* SYS_YIELD */
        "syscall\n\t"
        "decq %%r9\n\t"
        "jnz 1b\n\t"
        VERIFY_REGS
        "movq %%rax, %0\n\t"
        : "=m"(bad)
        :
        : "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13",
          "r14", "r15", "memory", "cc");
    return bad;
}

/* Spins on `flag` without a syscall; another thread sets it, which can only
   happen if the timer takes the CPU away from this loop. The message
   registers are held too, since an interrupt must save every register. */
static uint64_t spin_keeping_regs(void) {
    uint64_t bad;
    __asm__ volatile(
        SET_REGS
        "movq $0x6666, %%rsi\n\t"
        "movq $0x7777, %%r8\n"
        "1:\n\t"
        "pause\n\t"
        "cmpq $0, %1\n\t"
        "je 1b\n\t"
        VERIFY_REGS
        "movq %%rsi, %%rcx\n\t xorq $0x6666, %%rcx\n\t orq %%rcx, %%rax\n\t"
        "movq %%r8, %%rcx\n\t xorq $0x7777, %%rcx\n\t orq %%rcx, %%rax\n\t"
        "movq %%rax, %0\n\t"
        : "=m"(bad)
        : "m"(flag)
        : "rax", "rbx", "rcx", "rsi", "r8", "r12", "r13", "r14", "r15", "memory", "cc");
    return bad;
}

static void spinner(uint64_t which) {
    for (int round = 1; round <= 3; round++) {
        uint64_t until = sys_ticks() + 5;
        while (sys_ticks() < until) {}
        say(which ? "[archtest] spinner B round\n" : "[archtest] spinner A round\n");
    }
    __atomic_fetch_add(&spins_done, 1, __ATOMIC_SEQ_CST);
    sys_exit();
}

static void flagger(uint64_t arg) {
    (void)arg;
    flag = 1;
    sys_exit();
}

static void read_kernel(uint64_t arg) {
    (void)arg;
    __atomic_fetch_add(&faulters_ran, 1, __ATOMIC_SEQ_CST);
    say("[archtest] reading kernel memory from ring 3 (expect a page fault)\n");
    (void)*(volatile uint64_t *)0x100000;
    say("[archtest] FAIL: kernel memory was readable\n");
    sys_exit();
}

static void privileged(uint64_t arg) {
    (void)arg;
    __atomic_fetch_add(&faulters_ran, 1, __ATOMIC_SEQ_CST);
    say("[archtest] executing hlt in ring 3 (expect a general protection fault)\n");
    __asm__ volatile("hlt");
    say("[archtest] FAIL: hlt ran in ring 3\n");
    sys_exit();
}

/* ---- address spaces ---- */

/* Test-only syscalls that reach the arch VM layer directly (the real kernel
   gates these behind capabilities). */
enum { T_VSPACE = 20, T_FRAME = 21, T_MAP = 22, T_PEEK = 23, T_SPAWN = 25 };

#define V_DATA  0x8000000000ull        /* USER_VBASE: the private data page */
#define V_STACK 0x8000001000ull        /* and the private stack page */
#define V_GAP   0x8000002000ull        /* left unmapped, to fault on */

static volatile uint64_t space_ran;

/* Runs in its own address space. Writes its pattern to the private data
   page that both spaces map at the same V_DATA, then signals it is done.
   Its stack is V_STACK, private to its space. */
static void space_child(uint64_t pattern) {
    *(volatile uint64_t *)V_DATA = pattern;
    uint64_t back = *(volatile uint64_t *)V_DATA;
    if (back == pattern) __atomic_fetch_add(&space_ran, 1, __ATOMIC_SEQ_CST);
    sys_exit();
}

/* Touches a page its space never mapped: a page fault that kills it alone. */
static void space_faulter(uint64_t arg) {
    (void)arg;
    say("[archtest] a thread touches an unmapped page in its space (expect a fault)\n");
    *(volatile uint64_t *)V_GAP = 1;
    say("[archtest] FAIL: the unmapped page was writable\n");
    sys_exit();
}

static uint64_t raw(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    return sys6(nr, a0, a1, a2, a3, 0, 0, 0).value;   /* returns rdi */
}
static uint64_t raw_status(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    return sys6(nr, a0, a1, a2, a3, 0, 0, 0).status;  /* returns rax */
}

static void address_space_test(void) {
    uint64_t a = raw(T_VSPACE, 0, 0, 0, 0);
    uint64_t b = raw(T_VSPACE, 0, 0, 0, 0);
    check(a && b && a != b, "two address spaces are created");

    uint64_t fa_data = raw(T_FRAME, 0, 0, 0, 0), fa_stk = raw(T_FRAME, 0, 0, 0, 0);
    uint64_t fb_data = raw(T_FRAME, 0, 0, 0, 0), fb_stk = raw(T_FRAME, 0, 0, 0, 0);
    check(fa_data && fb_data && fa_data != fb_data, "frames come from the pool, each distinct");

    check(raw_status(T_MAP, a, V_DATA, fa_data, 1) == OK, "map a data page into space A");
    raw_status(T_MAP, a, V_STACK, fa_stk, 1);
    check(raw_status(T_MAP, b, V_DATA, fb_data, 1) == OK, "map a data page into space B at the same vaddr");
    raw_status(T_MAP, b, V_STACK, fb_stk, 1);
    check(raw_status(T_MAP, a, 0x1000, fa_data, 1) != OK, "mapping outside the private window is refused");

    raw_status(T_SPAWN, (uint64_t)space_child, V_STACK + 0xff0, 0xAAAAAAAAAAAAAAAAull, a);
    raw_status(T_SPAWN, (uint64_t)space_child, V_STACK + 0xff0, 0xBBBBBBBBBBBBBBBBull, b);
    while (space_ran < 2) sys_yield();

    check(raw(T_PEEK, fa_data, 0, 0, 0) == 0xAAAAAAAAAAAAAAAAull &&
          raw(T_PEEK, fb_data, 0, 0, 0) == 0xBBBBBBBBBBBBBBBBull,
          "the same vaddr reached different physical frames: the spaces are isolated");

    space_ran = 0;
    raw_status(T_SPAWN, (uint64_t)space_faulter, V_STACK + 0xff0, 0, a);
    for (int i = 0; i < 12; i++) sys_yield();   /* let the faulter run and die */
    raw_status(T_SPAWN, (uint64_t)space_child, V_STACK + 0xff0, 0xCCCCCCCCCCCCCCCCull, a);
    while (space_ran < 1) sys_yield();
    check(raw(T_PEEK, fa_data, 0, 0, 0) == 0xCCCCCCCCCCCCCCCCull,
          "a page fault killed one thread; its space survived for the next");
}

void user_main(void) {
    say("[archtest] entered ring 3\n");
    check(yield_keeping_regs() == 0, "callee-saved registers survive 200 syscalls (sysretq)");

    check(sys_thread_new(0, 0, spinner, stack_top(0), 1, 0) == OK, "thread creation");
    sys_thread_new(0, 0, spinner, stack_top(1), 1, 1);
    sys_thread_new(0, 0, flagger, stack_top(2), 1, 0);
    check(spin_keeping_regs() == 0, "the timer preempts ring 3 and every register survives (iretq)");

    sys_thread_new(0, 0, read_kernel, stack_top(3), 1, 0);
    sys_thread_new(0, 0, privileged, stack_top(4), 1, 0);
    while (spins_done < 2 || faulters_ran < 2) sys_yield();
    sys_yield();
    sys_yield();

    check(sys_ticks() > 0, "the timer ticks");
    check(sys_debug_write((const char *)0x100000, 8) != OK, "the console refuses kernel addresses");
    check(sys_thread_new(0, 0, flagger, (void *)0x100000, 1, 0) != OK,
          "a thread stack cannot be in kernel memory");

    address_space_test();

    say(failures ? "[archtest] FAIL: see above\n" : "[archtest] PASS\n");
    sys_exit();
}
