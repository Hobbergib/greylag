/* core.c -- a stand-in for the Goose core, for testing the arch layer on its
   own (make archtest). It implements k_init and k_trap with the same
   contract as kernel/kernel.goose, but only what the arch test needs: a
   round-robin scheduler, a timer, faults, and five syscalls. It is not the
   kernel; it exists so that boot, traps, the sysret and iretq paths, ring 3,
   preemption and the page protections can be checked without a Goose
   compiler. */

#include "hal.h"
#include "kernel_api.h"

#define NT 8

enum { T_FREE, T_READY, T_DEAD };

static int state[NT];
static uint32_t cur;
static uint64_t ticks;
static int slice;

void goose_init(int argc, char **argv) { (void)argc; (void)argv; }

uint32_t k_init(uint64_t root_entry, uint64_t root_stack, uint64_t user_lo, uint64_t user_hi) {
    (void)user_lo; (void)user_hi;
    thread_frame_init(0, (uint64_t)idle_thread_entry, (uint64_t)idle_stack_top, 0, 1);
    state[0] = T_READY;
    thread_frame_init(1, root_entry, root_stack, 0, 0);
    state[1] = T_READY;
    serial_puts("[archtest] C stand-in core, round robin over 8 threads\n");
    cur = 1;
    return cur;
}

/* The next ready thread after cur; the idle thread only if there is none. */
static uint32_t next_ready(void) {
    for (uint32_t i = 1; i <= NT; i++) {
        uint32_t t = (cur + i) % NT;
        if (t != 0 && state[t] == T_READY) return t;
    }
    return 0;
}

static void do_syscall(struct frame *f) {
    switch (f->rax) {
    case 0: /* debug write */
        if (f->rsi <= 4096 && user_range_ok(f->rdi, f->rsi)) {
            serial_write((const char *)f->rdi, f->rsi);
            f->rax = 0;
        } else {
            f->rax = 3;
        }
        break;
    case 1: /* yield */
        f->rax = 0;
        cur = next_ready();
        break;
    case 10: /* ticks */
        f->rdi = ticks;
        f->rax = 0;
        break;
    case 11: /* exit */
        state[cur] = T_DEAD;
        cur = next_ready();
        break;
    case 14: { /* thread new, started at once: rdx = entry, r10 = stack, r9 = arg */
        uint32_t t = 1;
        while (t < NT && state[t] != T_FREE) t++;
        if (t == NT || !user_range_ok(f->rdx, 1) || !user_range_ok(f->r10 - 16, 16)) {
            f->rax = 4;
            break;
        }
        thread_frame_init(t, f->rdx, f->r10, f->r9, 0);
        state[t] = T_READY;
        f->rax = 0;
        break;
    }
    default:
        f->rax = 8;
    }
}

uint32_t k_trap(uint64_t vector, uint64_t error, uint64_t fault_addr) {
    struct frame *f = frame_of(cur);
    if (vector == VEC_SYSCALL) {
        do_syscall(f);
    } else if (vector >= IRQ_BASE && vector < IRQ_BASE + NUM_IRQS) {
        pic_eoi((uint32_t)(vector - IRQ_BASE));
        if (vector == IRQ_BASE) {
            ticks++;
            if (++slice >= 2) {
                slice = 0;
                uint32_t n = next_ready();
                if (n) cur = n;
                else if (state[cur] != T_READY) cur = 0;
            }
        }
    } else {
        serial_puts("[archtest] thread ");
        serial_dec(cur);
        serial_puts(" killed by exception ");
        serial_dec(vector);
        serial_puts(" error ");
        serial_hex(error);
        serial_puts(" address ");
        serial_hex(fault_addr);
        serial_putc('\n');
        state[cur] = T_DEAD;
        cur = next_ready();
    }
    return cur;
}
