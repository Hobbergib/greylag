/* The kernel is identity-mapped at 1 MiB. Everything the user program is
   made of (user.o, whichever object provides it) goes into one 2 MiB-aligned
   span, the only memory paging_init opens to ring 3. */

ENTRY(_start)

SECTIONS
{
    . = 1M;

    .boot : {
        KEEP(*(.multiboot))
        *(.boot.text)
    }

    .text : {
        EXCLUDE_FILE(*user.o) *(.text .text.*)
    }

    .rodata : {
        EXCLUDE_FILE(*user.o) *(.rodata .rodata.*)
    }

    .data : {
        EXCLUDE_FILE(*user.o) *(.data .data.*)
    }

    .bss : {
        EXCLUDE_FILE(*user.o) *(.bss .bss.* COMMON)
    }

    . = ALIGN(2M);
    __user_start = .;
    .user : {
        *user.o(.text .text.* .rodata .rodata.* .data .data.* .bss .bss.* COMMON)
    }
    . = ALIGN(2M);
    __user_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
