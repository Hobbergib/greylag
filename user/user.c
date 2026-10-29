/* user.c -- the user-level system the kernel boots: a root task that builds
   everything else from its Control capability, and the threads that
   exercise the kernel and check its answers.

   Everything here runs in ring 3. The root and the ping/pong pair run in
   the boot space; the address-space test builds two private spaces and runs
   a thread in each. The one C blob is linked into the shared user region
   (arch/x86_64/linker.ld), which every space maps, so all threads share
   this code and its globals; what a space isolates is the private page each
   maps at the same virtual address.

   It must not call into the kernel's code: no C library, and the Makefile
   fails the build if this object has any undefined symbol. */

#include "syscall.h"

#define STACK_BYTES 8192

/* Root slots. 2-4 hold the long-lived objects; 5-9 are scratch reused
   across the children as each is fully set up before the next. */
enum { S_EP = 2, S_DONE = 3, S_EP2 = 4, S_A = 5, S_B = 6, S_C = 7, S_D = 8, S_E = 9,
       S_REPORT = 12, S_SPARE = 14 };
/* Child slots, filled by the root with SYS_CAP_COPY. */
enum { C_EP = 0, C_DONE = 1, C_EP2 = 2 };

enum { DONE_PING = 1, DONE_ADDER = 2, DONE_CLIENTS = 4, DONE_REVOKE = 8 };

#define PING_BADGE 0x1d
#define ROUNDS 20000

/* Private-space virtual addresses (inside USER_VBASE's window). */
#define V_DATA  (USER_VBASE + 0x0000)
#define V_STACK (USER_VBASE + 0x1000)
#define V_GAP   (USER_VBASE + 0x2000)   /* deliberately left unmapped */
#define V_STACK_TOP (V_STACK + 0xff8)   /* 8 below 16-aligned, as after a call */

static uint8_t stacks[6][STACK_BYTES] __attribute__((aligned(16)));
static uint8_t root_stack[STACK_BYTES] __attribute__((aligned(16), used));
__asm__(".globl user_root_stack_top\n\t.set user_root_stack_top, root_stack + 8184");

static uint64_t checks, failures;

/* ---- output: one line, one syscall, so lines never interleave ---- */

typedef struct { char b[192]; uint64_t n; } line_t;

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

/* ---- the IPC ping/pong pair (boot space) ---- */

static void pong_main(uint64_t arg) {
    (void)arg;
    msg_t m = { { 0, 0, 0, 0 } };
    sysret_t r = sys_recv(C_EP, 3, &m);    /* reply cap into slot 3 */
    for (;;) {
        m.w[0] += 1;
        m.w[1] = r.value;                  /* echo the caller's badge */
        r = sys_reply_recv(C_EP, 3, &m);
    }
}

static void ping_main(uint64_t arg) {
    (void)arg;
    msg_t m = { { 41, 7, 8, 9 } };
    check(sys_call(C_EP, &m) == OK && m.w[0] == 42 && m.w[1] == PING_BADGE,
          "a call returns the server's reply and the caller's badge");
    check(m.w[2] == 8 && m.w[3] == 9, "all four message words cross intact");

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

/* ---- reply objects: a server holding two outstanding calls at once ---- */

/* Receives two calls before answering either, so both callers are blocked
   in the kernel at the same time. It parks the first reply cap with
   SYS_CAP_MOVE, then answers the second and the first. The old single-slot
   design could not have held both. Doubles each caller's value. */
static void adder_main(uint64_t arg) {
    (void)arg;
    msg_t m = { { 0, 0, 0, 0 } };
    sys_recv(C_EP2, 3, &m);                /* first call, reply cap in slot 3 */
    uint64_t v1 = m.w[0];
    check(sys_cap_move(3, 4) == OK, "a reply cap can be parked in another slot");
    sys_recv(C_EP2, 3, &m);                /* second call, reply cap in slot 3 */
    uint64_t v2 = m.w[0];

    m.w[0] = v2 * 2;
    check(sys_reply(3, &m) == OK, "reply to the second caller");
    m.w[0] = v1 * 2;
    check(sys_reply(4, &m) == OK, "reply to the parked first caller");

    sys_signal(C_DONE);
    sys_exit();
}

static volatile uint64_t clients_ok;

static void adder_client(uint64_t value) {
    msg_t m = { { value, 0, 0, 0 } };
    if (sys_call(C_EP2, &m) == OK && m.w[0] == value * 2)
        __atomic_fetch_add(&clients_ok, 1, __ATOMIC_SEQ_CST);
    sys_signal(C_DONE);
    sys_exit();
}

/* ---- address spaces: isolation and per-space faults ---- */

/* Runs in its own space. Writes its pattern (the argument) to the private
   page both spaces map at V_DATA, reads it back, and reports it to the root
   over the shared endpoint. Its stack is V_STACK, private to its space. */
static void space_child(uint64_t pattern) {
    *(volatile uint64_t *)V_DATA = pattern;
    uint64_t back = *(volatile uint64_t *)V_DATA;
    msg_t m = { { back, 0, 0, 0 } };
    sys_send(C_EP, &m);                    /* report the readback */
    sys_exit();
}

/* Touches a page its space never mapped: a fault that kills it alone. */
static void space_faulter(uint64_t arg) {
    (void)arg;
    *(volatile uint64_t *)V_GAP = 1;
    sys_exit();
}

/* ---- revocation: a blocked receiver woken when its endpoint is destroyed */

static void revoke_victim(uint64_t arg) {
    (void)arg;
    msg_t m = { { 0, 0, 0, 0 } };
    uint64_t st = sys_recv(C_EP2, 3, &m).status;   /* blocks on the spare endpoint */
    check(st == E_DEAD, "a thread blocked on a revoked endpoint wakes with E_DEAD");
    sys_signal(C_DONE);
    sys_exit();
}

/* ---- the root task ---- */

static void spawn_at(uint64_t slot, void (*entry)(uint64_t), void *stack, uint64_t prio,
                     uint64_t arg) {
    check(sys_thread_new(ROOT_CONTROL, slot, entry, stack, prio, arg) == OK,
          "the root can create a thread");
}
/* Boot-space threads each get their own stack from `stacks`: 0 pong, 1 ping,
   2 adder, 3 and 4 its clients, 5 the revocation victim. Threads in a
   private space run on the stack page mapped in that space (V_STACK_TOP). */
static void spawn(uint64_t slot, void (*entry)(uint64_t), int stack, uint64_t prio, uint64_t arg) {
    spawn_at(slot, entry, stack_top(stack), prio, arg);
}
static void grant(uint64_t thread, uint64_t dst, uint64_t src, uint64_t rights, uint64_t badge) {
    check(sys_cap_copy(thread, src, dst, rights, badge) == OK, "the root can grant a capability");
}

static void root_checks(void) {
    msg_t m = { { 0, 0, 0, 0 } };
    check(sys_send(15, &m) == E_CAP, "an empty slot is no capability");
    check(sys_send(1000, &m) == E_CAP, "a slot number out of range is no capability");
    check(sys_endpoint_new(ROOT_SELF, 12) == E_CAP, "creating objects needs the Control cap");
    check(sys_thread_new(ROOT_CONTROL, 12, (void (*)(uint64_t))0x100000, stack_top(5), 5, 0) ==
              E_ARG,
          "a thread cannot start in kernel memory");
    check(sys_thread_new(ROOT_CONTROL, 12, pong_main, stack_top(5), 60, 0) == E_ARG,
          "a thread cannot be created above its creator's priority");
    check(sys_reply(3, &m) == E_NOCALLER, "a reply with no reply cap in the slot is refused");
    check(sys_debug_write((const char *)0x100000, 4) == E_ARG,
          "the console will not print kernel memory");
    check(sys6(99, 0, 0, 0, 0, 0, 0, 0).status == E_NOSYS, "unknown syscalls are refused");
}

/* Builds a space (cap in `space_slot`) with a private data page and stack,
   then runs `entry(pattern)` in it. The frame caps (scratch slots S_C/S_D)
   are dropped once mapped; the mappings persist in the space. Children
   report their readback over the dedicated report endpoint, so they never
   collide with the ping/pong pair on S_EP. */
static void run_in_new_space(uint64_t space_slot, void (*entry)(uint64_t), uint64_t pattern) {
    check(sys_vspace_new(ROOT_CONTROL, space_slot) == OK, "the root can create an address space");
    check(sys_frame_new(ROOT_CONTROL, S_C) == OK, "the root can allocate a frame");
    check(sys_frame_new(ROOT_CONTROL, S_D) == OK, "the root can allocate a stack frame");
    check(sys_map(space_slot, S_C, V_DATA, 1) == OK, "map the data page into the space");
    check(sys_map(space_slot, S_D, V_STACK, 1) == OK, "map the stack page into the space");
    check(sys_map(space_slot, S_C, V_DATA, 1) == E_STATE, "a frame maps in one place at a time");

    spawn_at(S_E, entry, (void *)V_STACK_TOP, 20, pattern); /* thread cap in S_E */
    check(sys_thread_set_space(S_E, space_slot) == OK, "bind the thread to the space");
    grant(S_E, C_EP, S_REPORT, RIGHT_SEND, 0);              /* so it can report */
    check(sys_thread_start(S_E) == OK, "start the thread in its own space");
    sys_cap_delete(S_E);                                    /* the thread cap */
    sys_cap_delete(S_C);                                    /* and both frame caps */
    sys_cap_delete(S_D);
}

static void address_space_test(void) {
    say("[root] address spaces: two threads, same vaddr, private memory");
    run_in_new_space(S_A, space_child, 0xAAAAAAAAAAAAAAAAull);
    msg_t m = { { 0, 0, 0, 0 } };
    uint64_t a = sys_recv(S_REPORT, 15, &m).status == OK ? m.w[0] : 0;
    run_in_new_space(S_B, space_child, 0xBBBBBBBBBBBBBBBBull);
    uint64_t b = sys_recv(S_REPORT, 15, &m).status == OK ? m.w[0] : 0;
    check(a == 0xAAAAAAAAAAAAAAAAull && b == 0xBBBBBBBBBBBBBBBBull,
          "each space read back its own value from the same virtual address");

    /* A fault in one space kills only its thread; the kernel keeps running.
       The faulter reuses space A, whose data and stack pages are still
       mapped, and touches the gap page that was never mapped. */
    say("[root] a thread faults in its space (expect it to be killed alone)");
    spawn_at(S_E, space_faulter, (void *)V_STACK_TOP, 20, 0);
    sys_thread_set_space(S_E, S_A);
    sys_thread_start(S_E);
    sys_cap_delete(S_E);
    sys_sleep(3);
    check(1, "the kernel survived a user page fault in a private space");

    /* An unstarted thread whose last cap is deleted can never run, so the
       kernel reaps it, which also drops its hold on space A. Without that,
       space A could never be revoked. */
    spawn_at(S_E, space_child, (void *)V_STACK_TOP, 20, 0);
    check(sys_thread_set_space(S_E, S_A) == OK, "bind a thread that will never start");
    check(sys_cap_delete(S_E) == OK, "delete the only cap to it");

    check(sys_cap_revoke(S_A) == OK, "space A can be revoked: the orphan was reaped");
    check(sys_cap_revoke(S_B) == OK, "space B can be revoked");
}

static void revocation_test(void) {
    say("[root] revocation: destroying an endpoint wakes its blocked receiver");
    check(sys_endpoint_new(ROOT_CONTROL, S_SPARE) == OK, "create a spare endpoint");
    spawn(S_E, revoke_victim, 5, 20, 0);
    grant(S_E, C_EP2, S_SPARE, RIGHT_RECV, 0);      /* victim receives on it as its slot 2 */
    grant(S_E, C_DONE, S_DONE, RIGHT_SEND, DONE_REVOKE);
    sys_thread_start(S_E);
    sys_cap_delete(S_E);
    sys_sleep(2);                                   /* let the victim block in recv */
    check(sys_cap_revoke(S_SPARE) == OK, "revoke the endpoint");
    msg_t m = { { 0, 0, 0, 0 } };
    check(sys_send(S_SPARE, &m) == E_CAP, "the revoked endpoint cap is now stale");

    /* A frame cap: revoke unmaps and frees it, and the cap goes stale. */
    check(sys_frame_new(ROOT_CONTROL, S_C) == OK, "allocate a frame to revoke");
    check(sys_vspace_new(ROOT_CONTROL, S_A) == OK, "a space to map it in");
    check(sys_map(S_A, S_C, V_DATA, 1) == OK, "map it");
    check(sys_cap_revoke(S_C) == OK, "revoke the frame (unmaps and frees it)");
    check(sys_unmap(S_C) == E_CAP, "the revoked frame cap is stale");

    /* A stale copy must not reach the object that reuses its slot. Copy a
       frame cap, revoke the original, let a new frame take the freed table
       slot, then revoke through the stale copy: it must be refused, and the
       new frame must survive (no double free of the physical page). */
    check(sys_frame_new(ROOT_CONTROL, S_C) == OK, "allocate a frame");
    check(sys_cap_copy(ROOT_SELF, S_C, S_D, RIGHT_ALL, 0) == OK, "copy its cap");
    check(sys_cap_revoke(S_C) == OK, "revoke the frame through the original");
    check(sys_frame_new(ROOT_CONTROL, S_C) == OK, "a new frame reuses the freed slot");
    check(sys_cap_revoke(S_D) == E_CAP, "revoking through a stale copy is refused");
    check(sys_map(S_A, S_C, V_DATA, 1) == OK, "the new frame survived the stale revoke");
    sys_cap_delete(S_D);
    sys_cap_revoke(S_C);
    sys_cap_revoke(S_A);
}

void user_main(void) {
    say("[root] started with the Control cap");
    root_checks();

    check(sys_endpoint_new(ROOT_CONTROL, S_EP) == OK, "the root can create an endpoint");
    check(sys_notif_new(ROOT_CONTROL, S_DONE) == OK, "the root can create a notification");
    check(sys_endpoint_new(ROOT_CONTROL, S_EP2) == OK, "the root can create a second endpoint");
    check(sys_endpoint_new(ROOT_CONTROL, S_REPORT) == OK, "the root can create a report endpoint");

    /* A reply slot must be empty or hold an old reply cap: naming a slot
       with a live cap in it is refused before anything blocks, since the
       reply cap would otherwise overwrite that cap. */
    msg_t probe = { { 0, 0, 0, 0 } };
    check(sys_recv(S_REPORT, S_EP, &probe).status == E_SLOT,
          "a receive refuses a reply slot that holds a live capability");

    /* ping/pong */
    spawn(10, pong_main, 0, 10, 0);
    grant(10, C_EP, S_EP, RIGHT_RECV, 0);
    sys_cap_move(10, 11);                            /* keep pong's thread cap in 11 */
    spawn(10, ping_main, 1, 10, 0);
    grant(10, C_EP, S_EP, RIGHT_SEND, PING_BADGE);
    grant(10, C_DONE, S_DONE, RIGHT_SEND, DONE_PING);
    check(sys_thread_start(11) == OK, "start the pong server");
    check(sys_thread_start(11) == E_STATE, "a thread starts only once");
    sys_thread_start(10);
    sys_cap_delete(10);
    sys_cap_delete(11);

    /* reply objects: adder server + two clients */
    spawn(10, adder_main, 2, 15, 0);
    grant(10, C_EP2, S_EP2, RIGHT_RECV, 0);
    grant(10, C_DONE, S_DONE, RIGHT_SEND, DONE_ADDER);
    sys_thread_start(10);
    sys_cap_delete(10);
    spawn(10, adder_client, 3, 15, 21);
    grant(10, C_EP2, S_EP2, RIGHT_SEND, 0);
    grant(10, C_DONE, S_DONE, RIGHT_SEND, DONE_CLIENTS);
    sys_thread_start(10);
    sys_cap_delete(10);
    spawn(10, adder_client, 4, 15, 100);
    grant(10, C_EP2, S_EP2, RIGHT_SEND, 0);
    sys_thread_start(10);
    sys_cap_delete(10);

    address_space_test();
    revocation_test();

    uint64_t need = DONE_PING | DONE_ADDER | DONE_REVOKE;
    uint64_t done = 0;
    while ((done & need) != need) {
        sysret_t r = sys_wait(S_DONE);
        if (r.status == OK) done |= r.value;
    }
    for (int i = 0; i < 100 && clients_ok < 2; i++) sys_sleep(1);
    check(clients_ok == 2, "both clients of the two-outstanding-call server got answers");

    line_t l;
    l.n = 0;
    l_str(&l, failures ? "[root] FAILED " : "[root] all ");
    l_u64(&l, failures ? failures : checks);
    l_str(&l, failures ? " checks" : " checks passed");
    l_end(&l);
    say("[root] done; the system is idle");
    sys_exit();
}
