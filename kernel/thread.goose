// Threads and the queues they wait in.
//
// Every thread control block and every queue sentinel lives in one
// grow-only global array, `tcbs`, built once at boot. The queue links are
// 4-byte offsets from that array's base (`Tcb&<u32 in tcbs>`): loading one
// is an add, storing one a subtract, and the compiler checks that a link
// can only ever point at a Tcb in tcbs. A thread in no queue links to
// itself, so linking and unlinking are always the same four stores.

enum TState {
    Free,          // an unused slot, on the free list
    Inactive,      // created, not started
    Ready,         // on its priority's run queue; the running thread is too
    Sending,       // on an endpoint's queue with a message
    Calling,       // the same, and will wait for a reply
    Receiving,     // on an endpoint's queue, waiting for a sender
    AwaitReply,    // its call was received; waits for a reply, in no queue
    Waiting,       // on a notification's queue
    Sleeping,      // on the sleep queue
    Queue,         // not a thread: a queue's sentinel
}

struct Tcb {
    id: u32,                       // its index in tcbs, and its frame in the arch layer
    state: TState,
    prio: u8,
    slice: u8,                     // ticks left in its time slice
    gen: u32,                      // bumped when the slot is freed: stale thread caps miss
    caprefs: u16,                  // live thread caps naming it (see cap_release)
    space: u32,                    // address space: 0 is the boot space, else spaces index + 1
    reply_slot: u8,                // where a Receiving thread wants its reply cap minted
    prev: Tcb&<u32 in tcbs>,
    next: Tcb&<u32 in tcbs>,
    badge: u64,                    // a queued sender's badge
    wake_at: u64,                  // a sleeper's tick
}

var tcbs: Tcb[>..] = [];
var cur_id: u32 = 0;

fn cur() -> Tcb& { tcbs[cur_id] }

fn push_tcb(state: TState) -> Tcb& {
    let id = tcbs.len as u32;
    return tcbs.push(Tcb { id: id, state: state, prio: 0, slice: 0, gen: 0, caprefs: 0, space: 0,
                           reply_slot: 0, prev: self, next: self, badge: 0, wake_at: 0 });
}

fn is_empty(q: Tcb&) -> bool { q.next .== q }

// t goes in front of pos; with pos a sentinel, that is the end of its queue.
fn link_before(pos: Tcb&, t: Tcb&) {
    t.prev .= pos.prev;
    t.next .= pos;
    pos.prev.next .= t;
    pos.prev .= t;
}

// t goes right after pos; with pos a sentinel, that is the front.
fn link_after(pos: Tcb&, t: Tcb&) {
    t.next .= pos.next;
    t.prev .= pos;
    pos.next.prev .= t;
    pos.next .= t;
}

fn unlink(t: Tcb&) {
    t.prev.next .= t.next;
    t.next.prev .= t.prev;
    t.prev .= t;
    t.next .= t;
}

// A free slot, taken off the free list.
fn alloc_thread() -> Tcb? {
    let fq .= tcbs[FREE_Q];
    guard !is_empty(fq) else { return null; }
    let t .= fq.next;
    unlink(t);
    return t;
}

// Ends t wherever it is: off its queue or run queue, every caller it owed a
// reply released with E_DEAD, its capabilities dropped, its hold on its
// address space released, its slot back on the free list under a new
// generation so stale caps to it miss.
fn kill(t: Tcb&) {
    if t.state == TState.Ready { make_unready(t); } else { unlink(t); }
    let n .= cnode(t);
    for &s in n.slots { drop_cap(s, E_DEAD); }
    release_space(t);
    t.caprefs = 0;
    t.gen += 1;
    t.state = TState.Free;
    link_before(tcbs[FREE_Q], t);
}

// An Inactive thread can only ever be started through a cap to it, so once
// its last thread cap is gone it can never run: it is reaped, which frees
// its slot and its hold on its address space. cap_release links such an
// orphan onto the reap queue and sets the flag; k_trap reaps at the end of
// the entry, so the common path pays one test and each orphan costs O(1).
// Killing one orphan drops the caps it held and may queue another, which
// the same loop then takes.
var reap_pending: bool = false;

fn reap_orphans() {
    let rq .= tcbs[REAP_Q];
    while !is_empty(rq) {
        let t .= rq.next;
        unlink(t);
        if t.state == TState.Inactive && t.caprefs == 0 { kill(t); }
    }
    reap_pending = false;
}

// A word printed in hex: print("rip ", Hex { v: rip }).
struct Hex { v: u64 }

fn format(out: u8[>..]&, h: Hex) {
    let digits = "0123456789abcdef";
    out.append("0x");
    for i in 0..16 {
        let shift = (15 - i) * 4;
        out.push(digits[(h.v >> shift) & 15]);
    }
}

// A user thread took an exception: report it and end the thread.
fn fault(t: Tcb&, vector: u64, error: u64, addr: u64) {
    print("[kernel] thread ", t.id, " killed by exception ", vector, " (error ", Hex { v: error },
          ") at rip ", Hex { v: hal_reg(t.id, REG_RIP) }, ", address ", Hex { v: addr });
    kill(t);
}
