# goose-microkernel

A small x86_64 microkernel whose core is written in
[Goose](https://github.com/aardappel/goose), Wouter van Oortmerssen's
memory-safe systems language with no heap.

It covers what a microkernel needs and nothing else: threads, a scheduler,
synchronous IPC, notifications, capabilities, timers, user-level interrupt
delivery and fault isolation. Drivers and services are meant to run in ring
3. The design aims for an efficient base: no allocation after boot, no
message copies through memory, O(1) hot paths, and a kernel entry that
saves registers without copying them.

```
kernel/          the core, in Goose: every policy decision
  kernel.goose     k_init and k_trap, the only two entry points
  thread.goose     thread table, intrusive queues, kill/fault
  sched.goose      priority bitmap scheduler, time slices, sleep queue
  ipc.goose        endpoints (call/reply), notifications, IRQ delivery
  cap.goose        capability nodes, rights, badges
  syscall.goose    syscall decoding
  abi.goose        syscall numbers, status codes, register slots
  config.goose     sizes and policy constants
  hal.goose        the extern fns the core uses to reach the machine
arch/x86_64/     the machine, in C and assembly
  boot.S           multiboot entry, 32 -> 64 bit, boot page tables
  entry.S          IST trap stubs, syscall entry, sysretq/iretq exits
  arch.c           GDT/TSS/IDT, paging, PIC, PIT, serial, trap dispatch
  hal.h            the register frame and the port/PIC/serial helpers
  goose_glue.h     HAL bodies for hal.goose + a freestanding Goose runtime
  klibc.c          memcpy/memset/calloc, no general heap
user/            the demo system (ring 3, C)
tools/archtest/  a C stand-in core for testing the arch layer alone
```

## How it fits together

**One split.** The arch layer saves the interrupted thread, calls
`k_trap(vector, error, fault_addr)`, and resumes whichever thread id that
returns. The Goose core never touches hardware except through the 11
`extern fn`s in `hal.goose`. Each of those is a `static inline` C function
compiled into the same C file as the kernel, so a call becomes a few
instructions in place.

**Zero-copy context save.** Every interrupt uses IST1, which always points
at the end of the current thread's frame in `arch_frames[]`. The CPU and
the stubs push the thread's registers straight into its own frame.
`syscall` builds the same frame by hand. A context switch is two stores
(the IST pointer and the syscall stack pointer), and the exit path pops the
next frame. A thread that last entered through `syscall` leaves through
`sysretq`; one that was interrupted leaves through `iretq`. The kernel runs
with interrupts off on a single kernel stack. It is never preempted and
never blocks, so there is no kernel state to save between entries.

**No heap, by construction.** Goose has no allocator. The kernel's objects
live in a fixed capability table (`cnodes`) and in grow-only global arrays
(`tcbs`, `eps`, `notifs`) that are built at boot and capped by
`config.goose`. Goose's data stacks are served by `goose_glue.h` from a
2 MiB arena of 64 KiB regions, each followed by an unmapped guard page. A
stack overrun therefore faults and is reported, instead of corrupting the
next region. The kernel is compiled with `--stack-reserve 64K` to match.

**Intrusive queues with 4-byte links.** Every thread and every queue
sentinel is a `Tcb` in `tcbs`. Queue links are `Tcb&<u32 in tcbs>`:
pool-relative offsets that the compiler checks can only point at a `Tcb`.
Queues are circular through a sentinel, so link and unlink are always the
same four stores, with no empty-list branches.

**Scheduler.** There are 64 strict priorities with round robin inside each.
`ready_mask` has one bit per non-empty run queue, and picking the next
thread is one `clz` plus a load. The running thread stays at the front of
its queue; when its slice runs out (2 ticks of 10 ms), it rotates to the
back. Priority 0 holds only the idle thread, which runs `sti; hlt` in
ring 0 and is saved like any other thread.

**IPC.** Endpoints are rendezvous points, in the style of seL4 and L4. A
message is four words in `rsi, rdx, r10, r8`, copied from the sender's
frame to the receiver's at the moment both are present. Nothing is
buffered. `SYS_CALL` sends and then waits for the reply. The receiver owes
exactly one reply, and `SYS_REPLY_RECV` gives a server one syscall per
request. When a thread blocks to hand the CPU to its partner (a call, or
reply-and-wait), the partner goes to the *front* of its run queue. A round
trip therefore never waits behind unrelated threads of the same priority.

**Capabilities.** Each thread has 16 slots. A slot holds a kind, rights
(SEND/RECV/GRANT), an object index and a badge. The root thread boots with
the only `Control` cap and builds the system from it: it creates
endpoints, notifications and threads, then hands out caps with
`SYS_CAP_COPY`, narrowing rights and stamping badges as it goes. A thread
cap carries the thread's generation, so a cap to a thread that has died
stops working instead of reaching whichever thread reuses the slot.

**Interrupts.** The kernel keeps only the timer. Any other IRQ line can be
bound to a notification (`SYS_IRQ_BIND`). The line is masked when it
fires and stays masked until the user-level driver acknowledges it with
`SYS_IRQ_ACK`, so a device that keeps asserting cannot keep the CPU in the
kernel.

**Faults.** A user exception kills only that thread. The kernel prints the
vector, error code, rip and fault address, any caller waiting on the
thread's reply is released with `E_DEAD`, and the slot returns to the free
list. An exception in ring 0 is a kernel panic with a register dump. A page
fault in the data-stack arena's guard pages is reported as a Goose stack
overflow.

The syscall table is in `kernel/abi.goose` (mirrored in `user/syscall.h`).

## Building and running

You need [goose](https://github.com/aardappel/goose) (a release, or built
from source with CMake), gcc, GNU binutils, and `qemu-system-x86_64`.

```bash
make            # build/kernel32.elf (GOOSE=/path/to/goose if it is not on PATH)
make run        # boot in QEMU, console on this terminal
make test       # boot headless, pass if the demo prints "[root] all N checks passed"
make check      # typecheck the Goose core only
make archtest   # the arch layer under a C stand-in core; needs no goose
```

The build runs `goose --stack-reserve 64K --include arch/x86_64/goose_glue.h
--header ... -o build/kernel.c kernel/kernel.goose`. It then compiles that
C freestanding, with `-mgeneral-regs-only`, so the kernel never touches the
user threads' SSE state. The result is linked with the arch layer and the
user demo. QEMU's multiboot loader only accepts 32-bit ELF files, so the
image is also written as `kernel32.elf`; the code inside is unchanged.

The demo (`user/user.c`) starts a root task, which runs about three dozen
checks:

- a ping/pong pair does 20,000 call/reply round trips and prints the cost
  per round trip in TSC cycles;
- a ticker thread checks that sleeps last as long as asked;
- two compute-bound threads at one priority take turns only because the
  timer preempts them;
- a thread writes to address 0 and is killed alone;
- the error paths: bad slots, missing rights, kernel addresses, priority
  escalation, stale thread caps and unknown syscalls.

Under QEMU's TCG emulation the cycle counts reflect the emulator, not
hardware.

## Status

What has been run:

- `make archtest` boots in QEMU and passes. It covers multiboot entry, long
  mode, the GDT/TSS/IDT, ring-3 entry, the `sysretq` path (callee-saved
  registers intact across 200 syscalls) and the `iretq` path. For `iretq`,
  a thread spinning with no syscalls is preempted by the timer and every
  register survives. It also covers user page faults, #GP on `hlt` in ring
  3, the idle thread, and the HAL's refusal of kernel addresses.
- `goose_glue.h` compiles against Goose's real `runtime.h`, with the
  kernel's flags and with the prototypes the compiler emits for `hal.goose`.
- `user/user.c` compiles with no undefined symbols.

What has not: **the Goose core in `kernel/` has not been compiled yet.** It
was written against the Goose reference, specification and samples. Each
construct it uses follows one that appears in Goose's own samples or tests.
Even so, expect `make check` to turn up a few type errors on first contact
with the compiler. It is a new language, and the core has not been through
it.

## Limits, and where to take it next

- **One address space.** User threads are isolated from the kernel (ring
  3, supervisor-only pages) but not from each other. The next step is a
  page-table object per address space, and a `space` field on `Tcb` that
  the arch layer switches CR3 on.
- **No revocation.** Caps can be copied but not deleted or revoked, and
  objects other than threads are never freed. A derivation tree, in the
  style of seL4's CDT, would fix this.
- **One reply per server.** A server that receives a second call before
  replying drops the first one, and that caller gets `E_STATE`. Reply
  objects would lift this.
- **Uniprocessor, periodic tick.** The kernel is UP-only. Tickless one-shot
  LAPIC timers and per-CPU run queues are the obvious next steps; the
  scheduler's data is already per-queue.
- **Bounds checks.** All indexing in the core is checked. Once it compiles,
  `goose --bce-lines` shows which checks survive. The IPC path is a
  candidate for `checkfree fn` with index types (`u32 in tcbs`), so the
  compiler proves the hot path check-free.
- **Short messages only.** Larger transfers would use shared pages, once
  address spaces exist.
