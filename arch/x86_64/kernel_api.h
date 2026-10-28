/* kernel_api.h -- the Goose kernel's entry points as the arch layer calls
   them. These are the `export fn`s in kernel/kernel.goose; goose_init comes
   from the generated C (spec §7.11). Kept by hand rather than included from
   the compiler's generated header, so arch.c builds without the generated
   type declarations. */

#ifndef GMK_KERNEL_API_H
#define GMK_KERNEL_API_H

#include <stdint.h>

/* Runs the Goose program's global initializers. Once, before anything else. */
void goose_init(int argc, char **argv);

/* Builds the kernel's objects and the root thread; returns the thread to run
   first. */
uint32_t k_init(uint64_t root_entry, uint64_t root_stack, uint64_t user_lo, uint64_t user_hi);

/* Every kernel entry after boot: a syscall (VEC_SYSCALL), an IRQ (32..47) or
   a user exception (0..31). The current thread's registers are already in
   its frame. Returns the thread to resume. */
uint32_t k_trap(uint64_t vector, uint64_t error, uint64_t fault_addr);

#endif
