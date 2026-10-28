// goose-microkernel: the kernel core.
//
// The arch layer (arch/x86_64) owns the machine: it boots, saves every
// interrupted thread's registers into that thread's frame, and resumes
// whichever thread the core names. This program owns every decision in
// between, through two entry points:
//
//   k_init  builds the kernel objects and the root thread, once;
//   k_trap  handles one kernel entry and returns the thread to run next.
//
// The core never allocates after boot and never copies a message through
// memory. Scheduling, IPC, notifications and capability lookups are O(1);
// the two walks are inserting into the sorted sleep queue (one step per
// earlier sleeper) and killing a thread (one pass over the thread table).
// Every object lives in a fixed table or a grow-only global array whose
// size is capped in config.goose.

import config;
import abi;
import hal;
import thread;
import sched;
import cap;
import ipc;
import syscall;

export fn k_init(root_entry: u64, root_stack: u64, user_lo: u64, user_hi: u64) -> u32 {
    for i in MAX_THREADS { push_tcb(TState.Free); }
    for i in NUM_PRIOS + 2 { push_tcb(TState.Queue); }      // run queues, free list, sleep queue
    let fq .= tcbs[FREE_Q];
    for i in 1..MAX_THREADS { link_before(fq, tcbs[i]); }

    let idle .= tcbs[IDLE];
    idle.prio = IDLE_PRIO;
    idle.slice = TIMESLICE;
    hal_idle_init(idle.id);
    make_ready(idle);

    let root = alloc_thread();
    assert(root);
    root.prio = ROOT_PRIO;
    root.slice = TIMESLICE;
    hal_thread_init(root.id, root_entry, root_stack, 0);
    let n .= cnode(root);
    n.slots[0] = Cap { kind: CapKind.Control, rights: RIGHT_ALL, obj: 0, badge: 0 };
    n.slots[1] = thread_cap(root);
    make_ready(root);

    print("[kernel] goose core up: ", MAX_THREADS, " threads, ", NUM_PRIOS, " priorities, ",
          CNODE_SLOTS, " cap slots per thread, user memory ", Hex { v: user_lo }, "..",
          Hex { v: user_hi });

    let first .= pick();
    cur_id = first.id;
    return first.id;
}

export fn k_trap(vector: u64, error: u64, fault_addr: u64) -> u32 {
    let c .= cur();
    match vector {
        VEC_SYSCALL => syscall(c),
        IRQ_BASE..IRQ_END => irq(vector - IRQ_BASE),
        _ => fault(c, vector, error, fault_addr),
    }
    let next .= pick();
    cur_id = next.id;
    return next.id;
}
