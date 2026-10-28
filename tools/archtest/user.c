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

    say(failures ? "[archtest] FAIL: see above\n" : "[archtest] PASS\n");
    sys_exit();
}
