# greylag

A small x86_64 microkernel whose core is written in
[Goose](https://github.com/aardappel/goose), Wouter van Oortmerssen's
memory-safe systems language with no heap.

It covers what a microkernel needs and nothing else: threads, a scheduler,
synchronous IPC with reply capabilities, notifications, a capability system
with generation-based revocation, per-thread address spaces, timers,
user-level interrupt delivery and fault isolation. Drivers and services are
meant to run in ring 3. The design aims for an efficient base: no allocation
after boot, no message copies through memory, O(1) hot paths, and a kernel
entry that saves registers without copying them.

```
kernel/          the core, in Goose: every policy decision
  kernel.goose     k_init and k_trap, the only two entry points
  thread.goose     thread table, intrusive queues, kill/fault
  sched.goose      priority bitmap scheduler, time slices, sleep queue
  ipc.goose        endpoints (call/reply caps), notifications, IRQ delivery
  cap.goose        capability nodes, rights, badges, generations
  vmem.goose       address spaces and frames over the arch page tables
  syscall.goose    syscall decoding
  abi.goose        syscall numbers, status codes, register slots
  config.goose     sizes and policy constants
  hal.goose        the extern fns the core uses to reach the machine
arch/x86_64/     the machine, in C and assembly
  boot.S           multiboot entry, 32 -> 64 bit, boot page tables
  entry.S          IST trap stubs, syscall entry, sysretq/iretq exits
  arch.c           GDT/TSS/IDT, paging, PIC, PIT, serial, trap dispatch, CR3 switch
  vmem.c           the physical frame pool and per-space page tables
  hal.h            the register frame and the port/PIC/serial helpers
  goose_glue.h     HAL bodies for hal.goose + a freestanding Goose runtime
  klibc.c          memcpy/memset/calloc, no general heap
user/            the demo system (ring 3, C)
tools/archtest/  a C stand-in core for testing the arch layer alone
```

## How it fits together

**One split.** The arch layer saves the interrupted thread, calls
`k_trap(vector, error, fault_addr)`, and resumes whichever thread id that
returns. The Goose core never touches hardware except through the 19
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
buffered. `SYS_CALL` sends and then waits for the reply. When a thread
blocks to hand the CPU to its partner (a call, or reply-and-wait), the
partner goes to the *front* of its run queue, so a round trip never waits
behind unrelated threads of the same priority.

**Reply capabilities.** Receiving a call mints a one-shot *reply capability*
into a slot the receiver names (in `r9`). The reply cap identifies the
waiting caller by id and generation. A server loop is one `SYS_REPLY_RECV`
per request, replying through that slot; but a server can also hold several
calls open at once by moving reply caps to parking slots with
`SYS_CAP_MOVE` and replying to each in its own time. A reply cap whose
caller has died, or that is deleted, resolves to no caller, so no call is
answered twice and no caller waits forever: if a server dies holding reply
caps, each owed caller is released with `E_DEAD`.

**Capabilities.** Each thread has 16 slots. A slot holds a kind, rights
(SEND/RECV/GRANT), a generation, an object index and a badge. The root
thread boots with the only `Control` cap and builds the system from it: it
creates endpoints, notifications, address spaces, frames and threads, then
hands out caps with `SYS_CAP_COPY`, narrowing rights and stamping badges as
it goes. `SYS_CAP_MOVE` moves a cap within a node and `SYS_CAP_DELETE`
clears a slot.

**Revocation by generation.** Every object carries a generation, and a cap
carries the generation it was minted at; a lookup checks they match.
`SYS_CAP_REVOKE` destroys the object — it bumps the generation and returns
the object to its table's free pool — so every existing cap to it, this one
and any copies, misses on its next use and is revoked at once. Only the
*original* cap, the one the object was created into, can revoke: it alone
carries `RIGHT_ORIGINAL`, which `SYS_CAP_COPY` always strips, so handing
someone a cap never hands them the power to destroy the object. There is no
derivation tree to walk: revocation is O(1), and a stale cap lingers
harmlessly in its slot until overwritten. Destroying an endpoint or
notification first releases every thread queued on it.

**Address spaces.** Each thread runs in one address space; the arch layer
loads its CR3 on every switch, changing it only when it differs. Every
space shares the kernel's low identity map (so a trap keeps the faulting
space's CR3 and still reaches the kernel), plus the shared user region, and
has a private 512 GiB window for its own pages. `SYS_VSPACE_NEW` makes a
space, `SYS_FRAME_NEW` takes a 4 KiB page from a fixed physical pool,
`SYS_MAP` maps a frame into a space's window, and `SYS_THREAD_SET_SPACE`
binds a not-yet-started thread to a space. A frame is mapped in one place
at a time, and the core remembers where, so unmap and revoke are clean.
Two threads mapping a frame each at the same virtual address get different
physical memory — the arch test proves exactly this.

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

The demo (`user/user.c`) starts a root task, which runs several dozen
checks:

- a ping/pong pair does 20,000 call/reply round trips and prints the cost
  per round trip in TSC cycles;
- an "adder" server receives two calls before answering either, parking the
  first reply cap with `SYS_CAP_MOVE` — two calls open at once, which the
  old single-reply design could not do;
- two threads, each in its own address space, map a private page at the
  same virtual address and read back different values — isolation;
- a thread faults in its space and is killed alone, and the kernel runs on;
- a thread blocked receiving on an endpoint is woken with `E_DEAD` when the
  endpoint is revoked, and a revoked frame cap goes stale;
- the error paths: bad slots, missing rights, kernel addresses, priority
  escalation and unknown syscalls.

Under QEMU's TCG emulation the cycle counts reflect the emulator, not
hardware.

## Status

What has been run:

- `make archtest` boots in QEMU and passes. It covers multiboot entry, long
  mode, the GDT/TSS/IDT, ring-3 entry, the `sysretq` path (callee-saved
  registers intact across 200 syscalls) and the `iretq` path. For `iretq`,
  a thread spinning with no syscalls is preempted by the timer and every
  register survives. It also covers the address-space layer end to end:
  creating two spaces, allocating frames, mapping a page into each at the
  same virtual address and reading back different physical memory, CR3
  switching on every thread switch, refusing a map outside the private
  window, a per-space page fault that kills one thread while its space
  carries on, user page faults, #GP on `hlt` in ring 3, the idle thread,
  and the HAL's refusal of kernel addresses.
- `goose_glue.h` compiles against Goose's real `runtime.h`, with the
  kernel's flags and with the prototypes the compiler emits for all 19
  `hal.goose` functions.
- `user/user.c` compiles with no undefined symbols.

What has not: **the Goose core in `kernel/` has not been compiled yet** —
building the Goose compiler was not possible in the environment this was
written in. The core was written against the Goose reference, specification
and samples, and each construct it uses follows one that appears in Goose's
own samples or tests. Even so, expect `make check` to turn up a few type
errors on first contact with the compiler. It is a new language, and the
core has not been through it. The arch layer, which `make archtest`
exercises under a C stand-in core, is the part that has actually run.

## Limits, and where to take it next

- **One shared user region for code.** Address spaces isolate each space's
  private pages, but the demo's threads still share one code/data blob
  mapped through the kernel's low map. Separate programs would need a loader
  that maps code and data into a fresh space from frames, rather than
  sharing the boot region.
- **No capability derivation tree.** Revocation is by generation: it
  invalidates every copy of a cap at once but cannot revoke only a subtree
  of derived caps. A proper mapping database (seL4's CDT) would add
  fine-grained, recursive revocation.
- **Orphan reaping counts, it doesn't trace.** A thread that was never
  started is reaped once the last cap to it is deleted. But an unstarted
  thread holding a cap to itself, or two unstarted threads holding caps to
  each other, form a cycle and are never reaped.
- **Uniprocessor, periodic tick.** The kernel is UP-only. Tickless one-shot
  LAPIC timers and per-CPU run queues are the obvious next steps; the
  scheduler's data is already per-queue.
- **Bounds checks.** All indexing in the core is checked. Once it compiles,
  `goose --bce-lines` shows which checks survive. The IPC path is a
  candidate for `checkfree fn` with index types (`u32 in tcbs`), so the
  compiler proves the hot path check-free.
- **Short messages only.** A message is four registers. Larger transfers
  would map a shared frame into both parties' spaces, which the frame and
  map objects already support.
- **A fixed frame pool.** `vmem.c` serves 2 MiB of frames from a static
  array. A real system would manage all of physical memory, discovered from
  the multiboot memory map.
