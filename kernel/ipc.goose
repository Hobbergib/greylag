// IPC: synchronous endpoints, asynchronous notifications, and IRQ delivery.
//
// An endpoint is a rendezvous. Whichever side arrives first waits in the
// endpoint's queue, which therefore holds only senders or only receivers.
// A message is the sender's four message registers, copied straight into
// the receiver's frame when both are there: no buffer, no copy through
// memory, and no step that depends on how many threads are waiting.
//
// A call is a send that then waits for a reply. The receiver becomes the
// caller's one owed reply (Tcb.caller); its next reply, or reply-and-receive,
// answers it.

struct Ep {
    queue: Tcb&<u32 in tcbs>,      // sentinel of the waiting threads
    receivers: bool,               // what a non-empty queue holds
}

struct Notif {
    queue: Tcb&<u32 in tcbs>,
    word: u64,                     // signalled bits nobody has taken yet
}

var eps: Ep[>..] = [];
var notifs: Notif[>..] = [];

fn new_endpoint() -> i64 {
    guard eps.len < MAX_ENDPOINTS else { return -1; }
    let q .= push_tcb(TState.Queue);
    eps.push(Ep { queue: q, receivers: false });
    return eps.len - 1;
}

fn new_notif() -> i64 {
    guard notifs.len < MAX_NOTIFS else { return -1; }
    let q .= push_tcb(TState.Queue);
    notifs.push(Notif { queue: q, word: 0 });
    return notifs.len - 1;
}

fn set_status(t: Tcb&, s: u64) { hal_set_reg(t.id, REG_RAX, s); }

// The message moves from s's frame to r's, with the badge r is told.
fn transfer(s: Tcb&, r: Tcb&, badge: u64) {
    hal_copy_msg(s.id, r.id);
    hal_set_reg(r.id, REG_RDI, badge);
    set_status(r, OK);
}

// r now owes c a reply. A reply r still owed (it received a call and never
// answered) is dropped, and that caller is released with E_STATE.
fn take_call(r: Tcb&, c: Tcb&) {
    let old = r.caller;
    if old {
        set_status(old, E_STATE);
        make_ready(old);
    }
    r.caller .= c;
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
            take_call(r, c);
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

fn ipc_recv(c: Tcb&, e: Ep&) {
    let q .= e.queue;
    if !is_empty(q) && !e.receivers {
        let s .= q.next;
        unlink(s);
        transfer(s, c, s.badge);
        if s.state == TState.Calling {
            s.state = TState.AwaitReply;
            take_call(c, s);
        } else {
            set_status(s, OK);
            make_ready(s);
        }
        return;
    }
    make_unready(c);
    c.state = TState.Receiving;
    link_before(q, c);
    e.receivers = true;
}

// Answers the call c took; false if it owes no reply. `first` hands the
// caller the CPU ahead of its peers, for a server about to block again.
fn ipc_reply(c: Tcb&, first: bool) -> bool {
    let r = c.caller;
    guard r else { return false; }
    c.caller .= null;
    hal_copy_msg(c.id, r.id);
    hal_set_reg(r.id, REG_RDI, 0);
    set_status(r, OK);
    if first { make_ready_first(r); } else { make_ready(r); }
    return true;
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

fn wait(c: Tcb&, n: Notif&) {
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
    if b.notif != 0 && b.notif <= notifs.len { signal(notifs[b.notif - 1], b.bits); }
}
