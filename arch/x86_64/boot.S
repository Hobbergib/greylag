/* boot.S -- Multiboot (v1) entry: from the loader's 32-bit protected mode
   into 64-bit long mode, then into kmain.

   The boot page tables identity-map the first 1 GiB with 2 MiB pages.
   arch.c's paging_init later edits these same tables: it opens the user
   region to ring 3 and gives the Goose data-stack arena 4 KiB pages with
   unmapped guard pages between its regions. */

#define MB_MAGIC 0x1badb002
#define MB_FLAGS 0x00000003 /* page-align modules, provide memory info */

    .section .multiboot, "a"
    .align 4
    .long MB_MAGIC
    .long MB_FLAGS
    .long -(MB_MAGIC + MB_FLAGS)

    .section .bss
    .align 4096
    .globl boot_pml4, boot_pdpt, boot_pd
boot_pml4:  .skip 4096
boot_pdpt:  .skip 4096
boot_pd:    .skip 4096
boot_stack: .skip 4096
boot_stack_top:

    .section .data
mb_magic: .long 0
mb_info:  .long 0

    .align 8
boot_gdt:
    .quad 0
    .quad 0x00af9a000000ffff /* 0x08: 64-bit kernel code */
    .quad 0x00cf92000000ffff /* 0x10: kernel data */
boot_gdt_end:
boot_gdt_ptr:
    .word boot_gdt_end - boot_gdt - 1
    .long boot_gdt

    .section .boot.text, "ax"
    .code32
    .globl _start
_start:
    cli
    movl $boot_stack_top, %esp
    movl %eax, mb_magic
    movl %ebx, mb_info

    /* Long mode needs CPUID leaf 0x80000001 bit 29. */
    movl $0x80000000, %eax
    cpuid
    cmpl $0x80000001, %eax
    jb no_long_mode
    movl $0x80000001, %eax
    cpuid
    btl $29, %edx
    jnc no_long_mode

    /* PML4[0] -> PDPT, PDPT[0] -> PD. The upper levels allow user access;
       each 2 MiB PDE decides it (paging_init sets U on the user region). */
    movl $boot_pdpt, %eax
    orl $0x7, %eax
    movl %eax, boot_pml4
    movl $boot_pd, %eax
    orl $0x7, %eax
    movl %eax, boot_pdpt

    /* PD[i] = i * 2 MiB | present | writable | 2 MiB page */
    xorl %ecx, %ecx
1:  movl %ecx, %eax
    shll $21, %eax
    orl $0x83, %eax
    movl %eax, boot_pd(, %ecx, 8)
    movl $0, boot_pd + 4(, %ecx, 8)
    incl %ecx
    cmpl $512, %ecx
    jne 1b

    movl %cr4, %eax
    orl $(1 << 5), %eax             /* PAE */
    movl %eax, %cr4

    movl $boot_pml4, %eax
    movl %eax, %cr3

    movl $0xc0000080, %ecx          /* EFER */
    rdmsr
    orl $((1 << 8) | (1 << 0)), %eax /* LME, SCE (syscall/sysret) */
    wrmsr

    movl %cr0, %eax
    orl $((1 << 31) | (1 << 16) | 1), %eax /* PG, WP, PE */
    movl %eax, %cr0

    lgdt boot_gdt_ptr
    ljmp $0x08, $long_mode_entry

no_long_mode:
    /* Nothing to print with yet: halt. */
    hlt
    jmp no_long_mode

    .code64
long_mode_entry:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    xorw %ax, %ax
    movw %ax, %fs
    movw %ax, %gs
    movq $kernel_stack_top, %rsp
    movl mb_magic, %edi
    movl mb_info, %esi
    call kmain
2:  cli
    hlt
    jmp 2b
