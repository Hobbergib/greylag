// The kernel's whole interface to the machine. Each of these is a static
// inline C function in arch/x86_64/goose_glue.h, compiled into the same C
// file as the kernel, so a call is a few instructions in place.
//
// Thread ids are plain u32s here (an index type cannot cross into C); the
// C side masks each one to MAX_THREADS, so a bad id cannot reach past the
// frames.

// A saved user register of thread t, by REG_* slot (abi.goose).
extern fn hal_reg(t: u32, slot: u32) -> u64;
// Sets one; slots past REG_R9 are ignored, so the core can never write a
// thread's rip, rsp or segments.
extern fn hal_set_reg(t: u32, slot: u32, v: u64);
// The four message registers, from one thread's frame to another's.
extern fn hal_copy_msg(from: u32, to: u32);

// A fresh ring-3 frame: entry point, stack top, and an argument in rdi.
extern fn hal_thread_init(t: u32, entry: u64, stack: u64, arg: u64);
// The idle thread's ring-0 frame.
extern fn hal_idle_init(t: u32);
// Whether [addr, addr + len) lies in user memory.
extern fn hal_user_range(addr: u64, len: u64) -> bool;
// Writes user bytes to the console; the count written, 0 if the range is bad.
extern fn hal_debug_write(addr: u64, len: u64) -> u64;

extern fn hal_irq_mask(irq: u32);
extern fn hal_irq_unmask(irq: u32);
extern fn hal_irq_eoi(irq: u32);

// Leading zero bits; 64 for 0.
extern fn hal_clz(x: u64) -> i64;
