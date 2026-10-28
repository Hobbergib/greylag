// Syscall decoding. Each handler validates its capability and arguments
// first, then changes state; none of them can fail halfway. A blocking
// handler leaves its result in the thread's frame for when it resumes.

// Argument n of the running syscall: rdi, rsi, rdx, r10, r8, r9.
fn arg(t: Tcb&, n: u32) -> u64 { hal_reg(t.id, n + 1) }

// A legal entry point or stack: in the shared user region, or in the private
// window of whatever address space the thread will run in (where an
// unmapped page simply faults and kills the thread, never the kernel).
fn addr_ok(a: u64) -> bool { hal_user_range(a, 1) || (a >= USER_VBASE && a < USER_VTOP) }

fn syscall(c: Tcb&) {
    match hal_reg(c.id, REG_RAX) {
        SYS_DEBUG_WRITE => sys_debug_write(c),
        SYS_YIELD => sys_yield(c),
        SYS_SEND => sys_send(c, false),
        SYS_CALL => sys_send(c, true),
        SYS_RECV => sys_recv(c),
        SYS_REPLY => sys_reply(c),
        SYS_REPLY_RECV => sys_reply_recv(c),
        SYS_SIGNAL => sys_signal(c),
        SYS_WAIT => sys_wait(c),
        SYS_SLEEP => sys_sleep(c),
        SYS_TICKS => sys_ticks(c),
        SYS_EXIT => kill(c),
        SYS_ENDPOINT_NEW => sys_object_new(c, CapKind.Endpoint),
        SYS_NOTIF_NEW => sys_object_new(c, CapKind.Notification),
        SYS_THREAD_NEW => sys_thread_new(c),
        SYS_THREAD_START => sys_thread_start(c),
        SYS_CAP_COPY => sys_cap_copy(c),
        SYS_IRQ_NEW => sys_irq_new(c),
        SYS_IRQ_BIND => sys_irq_bind(c),
        SYS_IRQ_ACK => sys_irq_ack(c),
        SYS_CAP_DELETE => sys_cap_delete(c),
        SYS_CAP_MOVE => sys_cap_move(c),
        SYS_CAP_REVOKE => sys_cap_revoke(c),
        SYS_VSPACE_NEW => sys_object_new(c, CapKind.Space),
        SYS_FRAME_NEW => sys_object_new(c, CapKind.Frame),
        SYS_MAP => sys_map(c),
        SYS_UNMAP => sys_unmap(c),
        SYS_THREAD_SET_SPACE => sys_thread_set_space(c),
        _ => set_status(c, E_NOSYS),
    }
}

fn sys_debug_write(c: Tcb&) {
    let len = arg(c, 1);
    let n = hal_debug_write(arg(c, 0), len);
    set_status(c, if n == len { OK } else { E_ARG });
}

fn sys_yield(c: Tcb&) {
    set_status(c, OK);
    rotate(c);
}

fn sys_send(c: Tcb&, calling: bool) {
    let cap = lookup(c, arg(c, 0), CapKind.Endpoint, RIGHT_SEND);
    guard cap else { set_status(c, E_CAP); return; }
    let e = ep_of(cap);
    guard e else { set_status(c, E_CAP); return; }
    ipc_send(c, e, cap.badge, calling);
}

fn sys_recv(c: Tcb&) {
    let cap = lookup(c, arg(c, 0), CapKind.Endpoint, RIGHT_RECV);
    guard cap else { set_status(c, E_CAP); return; }
    let e = ep_of(cap);
    guard e else { set_status(c, E_CAP); return; }
    let slot = arg(c, 5);
    guard slot < CNODE_SLOTS else { set_status(c, E_SLOT); return; }
    ipc_recv(c, e, slot as! u8);
}

fn sys_reply(c: Tcb&) {
    set_status(c, reply_via(c, arg(c, 0), false));
}

// A server's loop in one syscall per request: answer the caller named by
// the reply cap in `slot`, then wait for the next, minting its reply cap
// into the same slot.
fn sys_reply_recv(c: Tcb&) {
    let cap = lookup(c, arg(c, 0), CapKind.Endpoint, RIGHT_RECV);
    guard cap else { set_status(c, E_CAP); return; }
    let e = ep_of(cap);
    guard e else { set_status(c, E_CAP); return; }
    let slot = arg(c, 5);
    guard slot < CNODE_SLOTS else { set_status(c, E_SLOT); return; }
    reply_via(c, slot, true);
    ipc_recv(c, e, slot as! u8);
}

fn sys_signal(c: Tcb&) {
    let cap = lookup(c, arg(c, 0), CapKind.Notification, RIGHT_SEND);
    guard cap else { set_status(c, E_CAP); return; }
    let n = notif_of(cap);
    guard n else { set_status(c, E_CAP); return; }
    set_status(c, OK);
    signal(n, if cap.badge == 0 { 1 } else { cap.badge });
}

fn sys_wait(c: Tcb&) {
    let cap = lookup(c, arg(c, 0), CapKind.Notification, RIGHT_RECV);
    guard cap else { set_status(c, E_CAP); return; }
    let n = notif_of(cap);
    guard n else { set_status(c, E_CAP); return; }
    do_wait(c, n);
}

fn sys_sleep(c: Tcb&) {
    let n = arg(c, 0);
    set_status(c, OK);
    if n == 0 {
        rotate(c);
        return;
    }
    make_unready(c);
    sleep_until(c, ticks + n);
}

fn sys_ticks(c: Tcb&) {
    hal_set_reg(c.id, REG_RDI, ticks);
    set_status(c, OK);
}

// Creates an endpoint, notification, address space or frame, and puts a cap
// to it in an empty slot. All take a Control cap and an empty slot.
fn sys_object_new(c: Tcb&, kind: CapKind) {
    guard has_control(c, arg(c, 0)) else { set_status(c, E_CAP); return; }
    let dst = empty_slot(c, arg(c, 1));
    guard dst else { set_status(c, E_SLOT); return; }
    var obj: i64 = -1;
    var gen: u32 = 0;
    match kind {
        Endpoint => { obj = new_endpoint(); if obj >= 0 { gen = eps[obj].gen; } },
        Notification => { obj = new_notif(); if obj >= 0 { gen = notifs[obj].gen; } },
        Space => { obj = new_space(); if obj >= 0 { gen = spaces[obj].gen; } },
        Frame => { obj = new_frame(); if obj >= 0 { gen = frames[obj].gen; } },
        _ => {},
    }
    guard obj >= 0 else { set_status(c, E_FULL); return; }
    dst = Cap { kind: kind, rights: RIGHT_ALL, gen: gen, obj: obj as! u32, badge: 0 };
    set_status(c, OK);
}

// A thread starts Inactive, in the boot space, at a priority no higher than
// its creator's, with an empty capability node; its creator holds the only
// cap to it. Bind it to an address space (SYS_THREAD_SET_SPACE) before
// starting it to run it elsewhere.
fn sys_thread_new(c: Tcb&) {
    guard has_control(c, arg(c, 0)) else { set_status(c, E_CAP); return; }
    let dst = empty_slot(c, arg(c, 1));
    guard dst else { set_status(c, E_SLOT); return; }
    let entry = arg(c, 2);
    let stack = arg(c, 3);
    let prio = arg(c, 4);
    guard addr_ok(entry) && addr_ok(stack - 16) else { set_status(c, E_ARG); return; }
    guard prio >= 1 && prio <= c.prio else { set_status(c, E_ARG); return; }
    let t = alloc_thread();
    guard t else { set_status(c, E_FULL); return; }
    t.state = TState.Inactive;
    t.prio = prio as! u8;
    t.slice = TIMESLICE;
    t.space = 0;
    hal_thread_init(t.id, entry, stack, arg(c, 5));
    dst = thread_cap(t);
    set_status(c, OK);
}

fn sys_thread_start(c: Tcb&) {
    let cap = lookup(c, arg(c, 0), CapKind.Thread, 0);
    guard cap else { set_status(c, E_CAP); return; }
    let t = thread_of(cap);
    guard t else { set_status(c, E_CAP); return; }
    guard t.state == TState.Inactive else { set_status(c, E_STATE); return; }
    set_status(c, OK);
    make_ready(t);
}

fn sys_thread_set_space(c: Tcb&) {
    let tc = lookup(c, arg(c, 0), CapKind.Thread, 0);
    guard tc else { set_status(c, E_CAP); return; }
    let t = thread_of(tc);
    guard t else { set_status(c, E_CAP); return; }
    guard t.state == TState.Inactive && t.space == 0 else { set_status(c, E_STATE); return; }
    let sc = lookup(c, arg(c, 1), CapKind.Space, 0);
    guard sc else { set_status(c, E_CAP); return; }
    let s = space_of(sc);
    guard s else { set_status(c, E_CAP); return; }
    bind_space(t, s, sc.obj);
    set_status(c, OK);
}

// Copies one of the caller's caps into another thread's node. The copy keeps
// at most the source's rights, and only a cap with GRANT can be copied at
// all. An endpoint or notification cap without a badge takes the one given,
// which is how a server tells its clients apart; a badge, once set, stays.
fn sys_cap_copy(c: Tcb&) {
    let tc = lookup(c, arg(c, 0), CapKind.Thread, RIGHT_GRANT);
    guard tc else { set_status(c, E_CAP); return; }
    let dt = thread_of(tc);
    guard dt else { set_status(c, E_CAP); return; }
    let src_slot = arg(c, 1);
    guard src_slot < CNODE_SLOTS else { set_status(c, E_CAP); return; }
    let own .= cnode(c);
    let src .= own.slots[src_slot];
    guard src.kind != CapKind.Null && (src.rights & RIGHT_GRANT) != 0 else {
        set_status(c, E_CAP);
        return;
    }
    let dst = empty_slot(dt, arg(c, 2));
    guard dst else { set_status(c, E_SLOT); return; }
    var badge = src.badge;
    if badge == 0 && (src.kind == CapKind.Endpoint || src.kind == CapKind.Notification) {
        badge = arg(c, 4);
    }
    dst = Cap { kind: src.kind, rights: src.rights & (arg(c, 3) as! u8), gen: src.gen,
                obj: src.obj, badge: badge };
    set_status(c, OK);
}

fn sys_cap_delete(c: Tcb&) {
    let slot = arg(c, 0);
    guard slot < CNODE_SLOTS else { set_status(c, E_SLOT); return; }
    let own .= cnode(c);
    let s .= own.slots[slot];
    if s.kind == CapKind.Reply {
        let caller = thread_of(s);
        if caller { wake_await(caller, E_STATE); }
    }
    own.slots[slot] = NO_CAP;
    set_status(c, OK);
}

fn sys_cap_move(c: Tcb&) {
    let from = arg(c, 0);
    guard from < CNODE_SLOTS else { set_status(c, E_CAP); return; }
    let own .= cnode(c);
    let src .= own.slots[from];
    guard src.kind != CapKind.Null else { set_status(c, E_CAP); return; }
    let dst = empty_slot(c, arg(c, 1));
    guard dst else { set_status(c, E_SLOT); return; }
    dst = src;
    own.slots[from] = NO_CAP;
    set_status(c, OK);
}

// Destroys the object a cap names; every cap to it (this one and any copies)
// then misses on its generation. The invoking slot is cleared.
fn sys_cap_revoke(c: Tcb&) {
    let slot = arg(c, 0);
    guard slot < CNODE_SLOTS else { set_status(c, E_SLOT); return; }
    let own .= cnode(c);
    let s .= own.slots[slot];
    var status = OK;
    match s.kind {
        Endpoint => destroy_endpoint(s.obj),
        Notification => destroy_notif(s.obj),
        Frame => destroy_frame(s.obj),
        Space => { if !destroy_space(s.obj) { status = E_STATE; } },
        Thread => { let t = thread_of(s); if t { kill(t); } },
        Reply => { let caller = thread_of(s); if caller { wake_await(caller, E_STATE); } },
        _ => { status = E_CAP; },
    }
    if status == OK { own.slots[slot] = NO_CAP; }
    set_status(c, status);
}

fn sys_map(c: Tcb&) {
    let sc = lookup(c, arg(c, 0), CapKind.Space, 0);
    guard sc else { set_status(c, E_CAP); return; }
    let s = space_of(sc);
    guard s else { set_status(c, E_CAP); return; }
    let fc = lookup(c, arg(c, 1), CapKind.Frame, 0);
    guard fc else { set_status(c, E_CAP); return; }
    let f = frame_of(fc);
    guard f else { set_status(c, E_CAP); return; }
    set_status(c, map_frame(s, sc.obj, f, arg(c, 2), arg(c, 3) != 0));
}

fn sys_unmap(c: Tcb&) {
    let fc = lookup(c, arg(c, 0), CapKind.Frame, 0);
    guard fc else { set_status(c, E_CAP); return; }
    let f = frame_of(fc);
    guard f else { set_status(c, E_CAP); return; }
    set_status(c, unmap_frame(f));
}

fn sys_irq_new(c: Tcb&) {
    guard has_control(c, arg(c, 0)) else { set_status(c, E_CAP); return; }
    let dst = empty_slot(c, arg(c, 1));
    guard dst else { set_status(c, E_SLOT); return; }
    let line = arg(c, 2);
    guard line >= 1 && line < NUM_IRQS && line != 2 else { set_status(c, E_ARG); return; }
    dst = Cap { kind: CapKind.Irq, rights: RIGHT_ALL, gen: 0, obj: line as! u32, badge: 0 };
    set_status(c, OK);
}

fn sys_irq_bind(c: Tcb&) {
    let ic = lookup(c, arg(c, 0), CapKind.Irq, 0);
    guard ic else { set_status(c, E_CAP); return; }
    let nc = lookup(c, arg(c, 1), CapKind.Notification, RIGHT_SEND);
    guard nc else { set_status(c, E_CAP); return; }
    let n = notif_of(nc);
    guard n else { set_status(c, E_CAP); return; }
    irqs[ic.obj % NUM_IRQS] = IrqBinding { notif: nc.obj + 1,
                                           bits: if nc.badge == 0 { 1 } else { nc.badge } };
    hal_irq_unmask(ic.obj);
    set_status(c, OK);
}

fn sys_irq_ack(c: Tcb&) {
    let ic = lookup(c, arg(c, 0), CapKind.Irq, 0);
    guard ic else { set_status(c, E_CAP); return; }
    hal_irq_unmask(ic.obj);
    set_status(c, OK);
}
