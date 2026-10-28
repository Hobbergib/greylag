/* user.c -- the user-level system the kernel boots: a root task that builds
   everything else from its Control capability, and a handful of threads
   that exercise the kernel and check its answers.

   Everything here runs in ring 3. It is linked into the 2 MiB-aligned user
   region (see arch/x86_64/linker.ld) and must not call into the kernel's
   code: no C library, and the Makefile fails the build if this object has
   any undefined symbol (such as a memset the compiler might invent). */

#include "syscall.h"

#define STACK_BYTES 8192

/* Slots in the root's node. */
enum { S_EP = 2, S_DONE = 3, S_PONG = 4, S_PING = 5, S_TICKER = 6, S_SPIN_A = 7, S_SPIN_B = 8,
       S_FAULTY = 9 };
/* Slots in every child's node, filled by the root with SYS_CAP_COPY. */
enum { C_EP = 0, C_DONE = 1 };

/* Bits the children signal on the root's notification when they finish. */
enum { DONE_PING = 1, DONE_TICKER = 2, DONE_SPIN_A = 4, DONE_SPIN_B = 8, DONE_ALL = 15 };

#define PING_BADGE 0x1d
#define ROUNDS 20000

static uint8_t stacks[8][STACK_BYTES] __attribute__((aligned(16)));
static uint8_t root_stack[STACK_BYTES] __attribute__((aligned(16), used));
/* The root's initial rsp, for arch.c: 8 below the top, as after a call. */
__asm__(".globl user_root_stack_top\n\t.set user_root_stack_top, root_stack + 8184");

static uint64_t checks, failures;

/* ---- output: one line, one syscall, so lines never interleave ---- */

typedef struct { char b[160]; uint64_t n; } line_t;

static void l_str(line_t *l, const char *s) {
    while (*s && l->n < sizeof(l->b) - 1) l->b[l->n++] = *s++;
}
static void l_u64(line_t *l, uint64_t v) {
    char t[20];
    int k = 0;
    do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (k && l->n < sizeof(l->b) - 1) l->b[l->n++] = t[--k];
}
static void l_end(line_t *l) {
    l->b[l->n++] = '\n';
    sys_debug_write(l->b, l->n);
    l->n = 0;
}
static void say(const char *s) {
    line_t l;
    l.n = 0;
    l_str(&l, s);
    l_end(&l);
}

static void check(int ok, const char *what) {
    __atomic_fetch_add(&checks, 1, __ATOMIC_RELAXED);
    if (ok) return;
    __atomic_fetch_add(&failures, 1, __ATOMIC_RELAXED);
    line_t l;
    l.n = 0;
    l_str(&l, "[check] FAILED: ");
    l_str(&l, what);
    l_end(&l);
}

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static void *stack_top(int i) { return stacks[i] + STACK_BYTES - 8; }

/* ---- the IPC pair ---- */

/* Answers every call with the first word plus one and the caller's badge. */
static void pong_main(uint64_t arg) {
    (void)arg;
    msg_t m = { { 0, 0, 0, 0 } };
    sysret_t r = sys_recv(C_EP, &m);
    for (;;) {
        m.w[0] += 1;
        m.w[1] = r.value;
        r = sys_reply_recv(C_EP, &m);
    }
}

static void ping_main(uint64_t arg) {
    (void)arg;
    msg_t m = { { 41, 7, 8, 9 } };
    check(sys_call(C_EP, &m) == OK && m.w[0] == 42 && m.w[1] == PING_BADGE,
          "a call returns the server's reply and the caller's badge");
    check(m.w[2] == 8 && m.w[3] == 9, "all four message words cross intact");

    msg_t probe = { { 0, 0, 0, 0 } };
    check(sys_recv(C_EP, &probe).status == E_CAP, "a send-only endpoint cap cannot receive");

    uint64_t bad = 0;
    uint64_t t0 = rdtsc();
    for (uint64_t i = 0; i < ROUNDS; i++) {
        m.w[0] = i;
        sys_call(C_EP, &m);
        bad += m.w[0] != i + 1;
    }
    uint64_t t1 = rdtsc();
    check(bad == 0, "every round trip of the benchmark got its own answer");

    line_t l;
    l.n = 0;
    l_str(&l, "[ping] ");
    l_u64(&l, ROUNDS);
    l_str(&l, " call/reply round trips, ");
    l_u64(&l, (t1 - t0) / ROUNDS);
    l_str(&l, " TSC cycles each");
    l_end(&l);

    sys_signal(C_DONE);
    sys_exit();
}

/* ---- timers and preemption ---- */

static void ticker_main(uint64_t arg) {
    (void)arg;
    uint64_t start = sys_ticks();
    for (int i = 1; i <= 3; i++) {
        sys_sleep(10);
        uint64_t now = sys_ticks();
        line_t l;
        l.n = 0;
        l_str(&l, "[ticker] woke at tick ");
        l_u64(&l, now);
        l_end(&l);
        check(now >= start + (uint64_t)i * 10, "a sleep lasts at least as long as asked");
    }
    sys_signal(C_DONE);
    sys_exit();
}

/* Two compute-bound threads at one priority: each never blocks, so they
   only take turns because the timer preempts them when a slice runs out. */
static void spin_main(uint64_t which) {
    const char *name = which ? "B" : "A";
    for (int round = 1; round <= 3; round++) {
        uint64_t until = sys_ticks() + 12;
        while (sys_ticks() < until) {}
        line_t l;
        l.n = 0;
        l_str(&l, "[spin ");
        l_str(&l, name);
        l_str(&l, "] round ");
        l_u64(&l, (uint64_t)round);
        l_end(&l);
    }
    sys_signal(C_DONE);
    sys_exit();
}

/* ---- faults ---- */

static void faulty_main(uint64_t arg) {
    (void)arg;
    say("[faulty] writing to address 0");
    *(volatile uint64_t *)0 = 1;
    say("[faulty] still alive?");
    sys_exit();
}

/* ---- the root task ---- */

static void spawn(uint64_t slot, void (*entry)(uint64_t), int stack, uint64_t prio,
                  uint64_t arg) {
    check(sys_thread_new(ROOT_CONTROL, slot, entry, stack_top(stack), prio, arg) == OK,
          "the root can create a thread");
}

/* Gives a child the endpoint and the done-notification, each with only the
   rights it needs, and the badge that identifies it. */
static void grant(uint64_t thread, uint64_t ep_rights, uint64_t ep_badge, uint64_t done_bits) {
    if (ep_rights)
        check(sys_cap_copy(thread, S_EP, C_EP, ep_rights, ep_badge) == OK,
              "the root can grant an endpoint cap");
    if (done_bits)
        check(sys_cap_copy(thread, S_DONE, C_DONE, RIGHT_SEND, done_bits) == OK,
              "the root can grant a notification cap");
}

static void root_checks(void) {
    msg_t m = { { 0, 0, 0, 0 } };
    check(sys_send(15, &m) == E_CAP, "an empty slot is no capability");
    check(sys_send(1000, &m) == E_CAP, "a slot number out of range is no capability");
    check(sys_endpoint_new(ROOT_CONTROL, ROOT_SELF) == E_SLOT, "objects go only into empty slots");
    check(sys_endpoint_new(ROOT_SELF, 12) == E_CAP, "creating objects needs the Control cap");
    check(sys_thread_new(ROOT_CONTROL, 12, (void (*)(uint64_t))0x100000, stack_top(7), 5, 0) ==
              E_ARG,
          "a thread cannot start in kernel memory");
    check(sys_thread_new(ROOT_CONTROL, 12, pong_main, stack_top(7), 60, 0) == E_ARG,
          "a thread cannot be created above its creator's priority");
    check(sys_reply(&m) == E_NOCALLER, "a reply needs a call to answer");
    check(sys_debug_write((const char *)0x100000, 4) == E_ARG,
          "the console will not print kernel memory");
    check(sys_irq_new(ROOT_CONTROL, 12, 2) == E_ARG, "the PIC cascade line is not for drivers");
    check(sys6(99, 0, 0, 0, 0, 0, 0, 0).status == E_NOSYS, "unknown syscalls are refused");
}

void user_main(void) {
    say("[root] started with the Control cap");
    root_checks();

    check(sys_endpoint_new(ROOT_CONTROL, S_EP) == OK, "the root can create an endpoint");
    check(sys_notif_new(ROOT_CONTROL, S_DONE) == OK, "the root can create a notification");

    spawn(S_PONG, pong_main, 0, 10, 0);
    spawn(S_PING, ping_main, 1, 10, 0);
    spawn(S_TICKER, ticker_main, 2, 30, 0);
    spawn(S_SPIN_A, spin_main, 3, 5, 0);
    spawn(S_SPIN_B, spin_main, 4, 5, 1);
    spawn(S_FAULTY, faulty_main, 5, 40, 0);

    grant(S_PONG, RIGHT_RECV, 0, 0);
    grant(S_PING, RIGHT_SEND, PING_BADGE, DONE_PING);
    grant(S_TICKER, 0, 0, DONE_TICKER);
    grant(S_SPIN_A, 0, 0, DONE_SPIN_A);
    grant(S_SPIN_B, 0, 0, DONE_SPIN_B);

    check(sys_thread_start(S_PONG) == OK, "the root can start a thread");
    check(sys_thread_start(S_PONG) == E_STATE, "a thread starts only once");
    sys_thread_start(S_PING);
    sys_thread_start(S_TICKER);
    sys_thread_start(S_SPIN_A);
    sys_thread_start(S_SPIN_B);

    /* The faulting thread is the highest of the children, so it runs, and
       dies, as soon as the root sleeps. */
    sys_thread_start(S_FAULTY);
    sys_sleep(1);
    check(sys_thread_start(S_FAULTY) == E_CAP, "a cap to a dead thread is stale");

    uint64_t done = 0;
    while (done != DONE_ALL) {
        sysret_t r = sys_wait(S_DONE);
        check(r.status == OK && r.value != 0 && (r.value & ~(uint64_t)DONE_ALL) == 0,
              "a wait returns the bits that were signalled");
        done |= r.value;
    }

    line_t l;
    l.n = 0;
    l_str(&l, failures ? "[root] FAILED " : "[root] all ");
    l_u64(&l, failures ? failures : checks);
    l_str(&l, failures ? " checks" : " checks passed");
    l_end(&l);
    say("[root] done; the system is idle");
    sys_exit();
}
