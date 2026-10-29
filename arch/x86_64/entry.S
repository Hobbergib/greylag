/* entry.S -- every way into and out of the kernel.

   In: an interrupt or exception arrives on IST1, which always points at the
   end of the current thread's frame, so the CPU and the stubs below push
   the thread's state directly into arch_frames[current]. A syscall does the
   same by hand. Either way the frame is complete before any C runs, and
   then the kernel switches to its own stack.

   Out: trap_entry returns the frame of the thread to run next; restore pops
   it and leaves with sysretq if the thread last entered through syscall,
   iretq otherwise. The kernel runs with interrupts off and has one stack:
   it is never preempted and never blocks, so there is nothing to save
   between entries. */

#include "hal.h"

#define FRAME_VECTOR (15 * 8)

    .text
    .code64

/* Exceptions without a CPU error code get a 0 in its slot, so every frame
   has the same shape. */
.macro ISR_NOERR n, target=isr_common
    .align 16
isr\n:
    pushq $0
    pushq $\n
    jmp \target
.endm

.macro ISR_ERR n, target=isr_common
    .align 16
isr\n:
    pushq $\n
    jmp \target
.endm

.irp n, 0,1,3,4,5,6,7,9,15,16,19,20,22,23,24,25,26,27,28,31
    ISR_NOERR \n
.endr
.irp n, 10,11,12,13,14,17,21,29,30
    ISR_ERR \n
.endr
/* NMI, #MC and #DF arrive on IST2, the emergency stack, at any moment: an NMI
   is not masked by the cleared IF the kernel runs with, so it can land while
   the kernel is deep in k_trap on its one stack. Their handler therefore
   stays on IST2 instead of resetting to kernel_stack_top. */
    ISR_NOERR 2, isr_ist2
    ISR_NOERR 18, isr_ist2
    ISR_ERR 8, isr_ist2
/* The 16 PIC lines, remapped to 32..47. */
.irp n, 32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47
    ISR_NOERR \n
.endr

.macro SAVE_REGS
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
.endm

isr_common:
    SAVE_REGS
    movq %rsp, %rdi                 /* struct frame * */
    movq $kernel_stack_top, %rsp
    cld
    call trap_entry                 /* -> struct frame * to resume */
    movq %rax, %rsp
    jmp frame_restore

/* The frame sits at the top of the emergency stack, 16-byte aligned (the CPU
   aligns an IST stack, and a frame is 176 bytes); trap_entry runs below it
   on the same stack, touching neither the kernel stack nor any thread frame,
   and an NMI comes back here to resume whatever it interrupted. */
isr_ist2:
    SAVE_REGS
    movq %rsp, %rdi
    cld
    call trap_entry
    movq %rax, %rsp
    jmp frame_restore

/* syscall: rcx = user rip, r11 = user rflags, interrupts already masked by
   SFMASK. Build the same frame an interrupt from ring 3 would. One CPU and
   no kernel preemption, so a plain global can hold the user rsp for the
   few instructions until it is pushed. */
    .globl syscall_entry
syscall_entry:
    movq %rsp, syscall_user_rsp(%rip)
    movq cur_frame_end(%rip), %rsp
    pushq $USER_DS
    pushq syscall_user_rsp(%rip)
    pushq %r11
    pushq $USER_CS
    pushq %rcx
    pushq $0
    pushq $VEC_SYSCALL
    jmp isr_common

/* rdi = the frame to resume. */
    .globl arch_enter
arch_enter:
    movq %rdi, %rsp
    /* fall through */

frame_restore:
    cmpq $VEC_SYSCALL, FRAME_VECTOR(%rsp)
    je 1f
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    addq $16, %rsp                  /* vector, error */
    iretq

    /* The fast way back for a thread that entered through syscall: rip and
       rflags go back through rcx and r11, which the syscall ABI clobbers
       anyway. A syscall frame always holds the canonical user rip the CPU
       itself saved, so sysretq cannot fault in ring 0 here. */
1:  popq %r15
    popq %r14
    popq %r13
    popq %r12
    addq $8, %rsp                   /* r11: replaced by rflags */
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    addq $8, %rsp                   /* rcx: replaced by rip */
    popq %rbx
    popq %rax
    movq 16(%rsp), %rcx             /* rip    (vector at 0, error at 8) */
    movq 32(%rsp), %r11             /* rflags (cs at 24) */
    movq 40(%rsp), %rsp             /* user rsp */
    sysretq

/* The idle thread runs in ring 0 and never touches its stack: the next
   interrupt saves it into its own frame like any other thread. */
    .globl idle_thread_entry
idle_thread_entry:
    sti
    hlt
    jmp idle_thread_entry

    .section .rodata
    .align 8
    .globl isr_stub_table
isr_stub_table:
.irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47
    .quad isr\n
.endr

    .bss
    .align 16
    .globl kernel_stack_top, idle_stack_top
kernel_stack:
    .skip 16384
kernel_stack_top:
idle_stack:
    .skip 512
idle_stack_top:

    .section .note.GNU-stack, "", @progbits
