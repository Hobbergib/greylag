/* syscall.h -- the user side of kernel/abi.goose. Keep the two in step. */

#ifndef GMK_SYSCALL_H
#define GMK_SYSCALL_H

#include <stdint.h>

enum {
    SYS_DEBUG_WRITE = 0, SYS_YIELD, SYS_SEND, SYS_CALL, SYS_RECV, SYS_REPLY, SYS_REPLY_RECV,
    SYS_SIGNAL, SYS_WAIT, SYS_SLEEP, SYS_TICKS, SYS_EXIT, SYS_ENDPOINT_NEW, SYS_NOTIF_NEW,
    SYS_THREAD_NEW, SYS_THREAD_START, SYS_CAP_COPY, SYS_IRQ_NEW, SYS_IRQ_BIND, SYS_IRQ_ACK,
};

enum {
    OK = 0, E_CAP, E_SLOT, E_ARG, E_FULL, E_STATE, E_NOCALLER, E_DEAD, E_NOSYS,
};

enum { RIGHT_SEND = 1, RIGHT_RECV = 2, RIGHT_GRANT = 4, RIGHT_ALL = 7 };

/* The root thread's node at boot. */
enum { ROOT_CONTROL = 0, ROOT_SELF = 1 };

/* An IPC message: four words, carried in rsi, rdx, r10 and r8. */
typedef struct { uint64_t w[4]; } msg_t;

/* What every syscall returns: the status (rax) and the value (rdi). */
typedef struct { uint64_t status, value; } sysret_t;

static inline sysret_t sys6(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, msg_t *m) {
    register uint64_t rax __asm__("rax") = nr;
    register uint64_t rdi __asm__("rdi") = a0;
    register uint64_t rsi __asm__("rsi") = a1;
    register uint64_t rdx __asm__("rdx") = a2;
    register uint64_t r10 __asm__("r10") = a3;
    register uint64_t r8 __asm__("r8") = a4;
    register uint64_t r9 __asm__("r9") = a5;
    __asm__ volatile("syscall"
                     : "+r"(rax), "+r"(rdi), "+r"(rsi), "+r"(rdx), "+r"(r10), "+r"(r8), "+r"(r9)
                     :
                     : "rcx", "r11", "memory");
    if (m) {
        m->w[0] = rsi;
        m->w[1] = rdx;
        m->w[2] = r10;
        m->w[3] = r8;
    }
    return (sysret_t){ rax, rdi };
}

/* A message-carrying syscall: m goes out in the message registers and comes
   back holding whatever the kernel put there. */
static inline sysret_t sys_msg(uint64_t nr, uint64_t cap, msg_t *m) {
    return sys6(nr, cap, m->w[0], m->w[1], m->w[2], m->w[3], 0, m);
}

static inline uint64_t sys_debug_write(const char *s, uint64_t n) {
    return sys6(SYS_DEBUG_WRITE, (uint64_t)s, n, 0, 0, 0, 0, 0).status;
}
static inline void sys_yield(void) { sys6(SYS_YIELD, 0, 0, 0, 0, 0, 0, 0); }
static inline uint64_t sys_send(uint64_t ep, msg_t *m) { return sys_msg(SYS_SEND, ep, m).status; }
static inline uint64_t sys_call(uint64_t ep, msg_t *m) { return sys_msg(SYS_CALL, ep, m).status; }
static inline sysret_t sys_recv(uint64_t ep, msg_t *m) { return sys_msg(SYS_RECV, ep, m); }
static inline uint64_t sys_reply(msg_t *m) { return sys_msg(SYS_REPLY, 0, m).status; }
static inline sysret_t sys_reply_recv(uint64_t ep, msg_t *m) { return sys_msg(SYS_REPLY_RECV, ep, m); }
static inline uint64_t sys_signal(uint64_t n) { return sys6(SYS_SIGNAL, n, 0, 0, 0, 0, 0, 0).status; }
static inline sysret_t sys_wait(uint64_t n) { return sys6(SYS_WAIT, n, 0, 0, 0, 0, 0, 0); }
static inline void sys_sleep(uint64_t ticks) { sys6(SYS_SLEEP, ticks, 0, 0, 0, 0, 0, 0); }
static inline uint64_t sys_ticks(void) { return sys6(SYS_TICKS, 0, 0, 0, 0, 0, 0, 0).value; }
static inline void sys_exit(void) {
    sys6(SYS_EXIT, 0, 0, 0, 0, 0, 0, 0);
    for (;;) {}
}
static inline uint64_t sys_endpoint_new(uint64_t ctl, uint64_t slot) {
    return sys6(SYS_ENDPOINT_NEW, ctl, slot, 0, 0, 0, 0, 0).status;
}
static inline uint64_t sys_notif_new(uint64_t ctl, uint64_t slot) {
    return sys6(SYS_NOTIF_NEW, ctl, slot, 0, 0, 0, 0, 0).status;
}
static inline uint64_t sys_thread_new(uint64_t ctl, uint64_t slot, void (*entry)(uint64_t),
                                      void *stack_top, uint64_t prio, uint64_t arg) {
    return sys6(SYS_THREAD_NEW, ctl, slot, (uint64_t)entry, (uint64_t)stack_top, prio, arg, 0)
        .status;
}
static inline uint64_t sys_thread_start(uint64_t thread) {
    return sys6(SYS_THREAD_START, thread, 0, 0, 0, 0, 0, 0).status;
}
static inline uint64_t sys_cap_copy(uint64_t thread, uint64_t src, uint64_t dst, uint64_t rights,
                                    uint64_t badge) {
    return sys6(SYS_CAP_COPY, thread, src, dst, rights, badge, 0, 0).status;
}
static inline uint64_t sys_irq_new(uint64_t ctl, uint64_t slot, uint64_t line) {
    return sys6(SYS_IRQ_NEW, ctl, slot, line, 0, 0, 0, 0).status;
}
static inline uint64_t sys_irq_bind(uint64_t irq, uint64_t notif) {
    return sys6(SYS_IRQ_BIND, irq, notif, 0, 0, 0, 0, 0).status;
}
static inline uint64_t sys_irq_ack(uint64_t irq) {
    return sys6(SYS_IRQ_ACK, irq, 0, 0, 0, 0, 0, 0).status;
}

#endif
