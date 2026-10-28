// IPC: synchronous endpoints, asynchronous notifications, and IRQ delivery.
//
// An endpoint is a rendezvous. Whichever side arrives first waits in the
// endpoint's queue, which therefore holds only senders or only receivers.
// A message is the sender's four message registers, copied straight into
// the receiver's frame when both are present: no buffer, no copy through
// memory, and no step that depends on how many threads are waiting.
//
// A call is a send that then waits for a reply. Receiving a call mints a
// one-shot *reply capability* into a slot the receiver names. The reply cap
// identifies the waiting caller by its id and generation, so a server can
// hold several outstanding calls by moving reply caps to parking slots
// (SYS_CAP_MOVE) and replying to each in its own time. A reply cap whose
// caller has died, or that is deleted, resolves to no caller, so no call is
// ever answered twice and no caller waits on a reply that will never come.

struct Ep {
    queue: Tcb&<u32 in tcbs>,      // sentinel of the waiting threads
    receivers: bool,               // what a non-empty queue holds
    gen: u32,
    live: bool,
}

struct Notif {
    queue: Tcb&<u32 in tcbs>,
    word: u64,                     // signalled bits nobody has taken yet
    gen: u32,
    live: bool,
}

var eps: Ep[>..] = [];
var notifs: Notif[>..] = [];

// A free slot in the table (reusing a destroyed one), or a fresh push.
fn new_endpoint() -> i64 {
    for i in eps.len {
        let e .= eps[i];
        if !e.live { e.live = true; e.receivers = false; return i; }
    }
    guard eps.len < MAX_ENDPOINTS else { return -1; }
    let q .= push_tcb(TState.Queue);
    eps.push(Ep { queue: q, receivers: false, gen: 0, live: true });
    return eps.len - 1;
}

fn new_notif() -> i64 {
    for i in notifs.len {
        let n .= notifs[i];
        if !n.live { n.live = true; n.word = 0; return i; }
    }
    guard notifs.len < MAX_NOTIFS else { return -1; }
    let q .= push_tcb(TState.Queue);
    notifs.push(Notif { queue: q, word: 0, gen: 0, live: true });
    return notifs.len - 1;
}

// The object a cap names, checked live and at the right generation.
fn ep_of(c: Cap&) -> Ep? {
    guard c.obj < eps.len else { return null; }
    let e .= eps[c.obj];
    guard e.live && e.gen == c.gen else { return null; }
    return e;
}

fn notif_of(c: Cap&) -> Notif? {
    guard c.obj < notifs.len else { return null; }
    let n .= notifs[c.obj];
    guard n.live && n.gen == c.gen else { return null; }
    return n;
}

fn set_status(t: Tcb&, s: u64) { hal_set_reg(t.id, REG_RAX, s); }

// Wakes a thread that is waiting for a reply, with a status and no message.
fn wake_await(t: Tcb&, status: u64) {
    if t.state == TState.AwaitReply {
        set_status(t, status);
        make_ready(t);
    }
}

// The message moves from s's frame to r's, with the badge r is told.
fn transfer(s: Tcb&, r: Tcb&, badge: u64) {
    hal_copy_msg(s.id, r.id);
    hal_set_reg(r.id, REG_RDI, badge);
    set_status(r, OK);
}

// Mints a one-shot reply cap naming `caller` into `server`'s slot. If the
// slot already holds a live reply cap, that older caller can no longer be
// answered through this slot, so it is released with E_STATE first.
fn mint_reply(server: Tcb&, slot: u8, caller: Tcb&) {
    let n .= cnode(server);
    let old .= n.slots[slot % CNODE_SLOTS];
    if old.kind == CapKind.Reply {
        let oc = thread_of(old);
        if oc { wake_await(oc, E_STATE); }
    }
    n.slots[slot % CNODE_SLOTS] = Cap { kind: CapKind.Reply, rights: 0, gen: caller.gen,
                                        obj: caller.id, badge: 0 };
}

// c, the running thread, sends on e: SYS_SEND, or SYS_CALL when calling.
fn ipc_send(c: Tcb&, e: Ep&, badge: u64, calling: bool) {
    let q .= e.queue;
    if !is_empty(q) && e.receivers {
        let r .= q.next;
        unlink(r);
        transfer(c, r, badge);
        if calling {
            make_unready(c);
            c.state = TState.AwaitReply;
            mint_reply(r, r.reply_slot, c);
            make_ready_first(r);
        } else {
            set_status(c, OK);
            make_ready(r);
        }
        return;
    }
    make_unready(c);
    c.state = if calling { TState.Calling } else { TState.Sending };
    c.badge = badge;
    link_before(q, c);
    e.receivers = false;
}

// c receives on e, minting a reply cap for a received call into `reply_slot`.
fn ipc_recv(c: Tcb&, e: Ep&, reply_slot: u8) {
    let q .= e.queue;
    if !is_empty(q) && !e.receivers {
        let s .= q.next;
        unlink(s);
        transfer(s, c, s.badge);
        if s.state == TState.Calling {
            s.state = TState.AwaitReply;
            mint_reply(c, reply_slot, s);
        } else {
            set_status(s, OK);
            make_ready(s);
        }
        return;
    }
    make_unready(c);
    c.state = TState.Receiving;
    c.reply_slot = reply_slot;
    link_before(q, c);
    e.receivers = true;
}

// Answers the caller named by the reply cap in c's slot, and clears it (a
// reply cap is one-shot). `first` hands the caller the CPU ahead of its
// peers, for a server about to block again.
fn reply_via(c: Tcb&, slot: u64, first: bool) -> u64 {
    let n .= cnode(c);
    let rc .= n.slots[slot % CNODE_SLOTS];
    guard rc.kind == CapKind.Reply else { return E_NOCALLER; }
    let caller = thread_of(rc);
    n.slots[slot % CNODE_SLOTS] = NO_CAP;
    guard caller else { return E_DEAD; }
    guard caller.state == TState.AwaitReply else { return E_STATE; }
    hal_copy_msg(c.id, caller.id);
    hal_set_reg(caller.id, REG_RDI, 0);
    set_status(caller, OK);
    if first { make_ready_first(caller); } else { make_ready(caller); }
    return OK;
}

// Destroys an endpoint: every thread queued on it is released with E_DEAD,
// and the slot is freed under a new generation.
fn destroy_endpoint(i: u32) {
    guard i < eps.len else { return; }
    let e .= eps[i];
    guard e.live else { return; }          // already destroyed: nothing to do twice
    let q .= e.queue;
    while !is_empty(q) {
        let w .= q.next;
        unlink(w);
        set_status(w, E_DEAD);
        make_ready(w);
    }
    e.live = false;
    e.gen += 1;
}

fn destroy_notif(i: u32) {
    guard i < notifs.len else { return; }
    let n .= notifs[i];
    guard n.live else { return; }
    let q .= n.queue;
    while !is_empty(q) {
        let w .= q.next;
        unlink(w);
        hal_set_reg(w.id, REG_RDI, 0);
        set_status(w, E_DEAD);
        make_ready(w);
    }
    n.live = false;
    n.gen += 1;
}

// Sets bits in n's word; a waiting thread takes the whole word at once.
fn signal(n: Notif&, bits: u64) {
    let q .= n.queue;
    if is_empty(q) {
        n.word |= bits;
        return;
    }
    let w .= q.next;
    unlink(w);
    hal_set_reg(w.id, REG_RDI, n.word | bits);
    set_status(w, OK);
    n.word = 0;
    make_ready(w);
}

fn do_wait(c: Tcb&, n: Notif&) {
    if n.word != 0 {
        hal_set_reg(c.id, REG_RDI, n.word);
        set_status(c, OK);
        n.word = 0;
        return;
    }
    make_unready(c);
    c.state = TState.Waiting;
    link_before(n.queue, c);
}

// IRQ lines other than the timer are delivered to user-level drivers as
// notification signals. A line stays masked from its interrupt until the
// driver acknowledges it (SYS_IRQ_ACK), so a device that keeps asserting
// cannot keep the CPU in the kernel.
struct IrqBinding { notif: u32, bits: u64 }     // notif: index + 1; 0 is unbound

var irqs: IrqBinding[NUM_IRQS];

fn irq(line: u64) {
    if line == 0 {
        hal_irq_eoi(0);
        tick();
        return;
    }
    let l = line as u32;
    hal_irq_mask(l);
    hal_irq_eoi(l);
    let b = irqs[line % NUM_IRQS];
    if b.notif != 0 && b.notif <= notifs.len {
        let n .= notifs[b.notif - 1];
        if n.live { signal(n, b.bits); }
    }
}
