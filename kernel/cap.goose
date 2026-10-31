// Capabilities. A thread names a kernel object only through a slot of its
// own capability node, and uses it only as far as the slot's rights allow.
// A slot is a small fixed value in a per-thread table, so a lookup is one
// range check on the slot number and one load.
//
// Revocation is by generation. Every object carries a generation, and a cap
// carries the generation it was minted at. Destroying an object bumps its
// generation and returns it to its table's free pool, so every existing cap
// to it fails its generation check on the next use and is revoked at once,
// with no derivation tree to walk. A stale cap lingers harmlessly in its
// slot until overwritten; it can never reach the object that replaced it.
//
// The root thread boots with the only Control cap, which creates objects,
// and hands out what it chooses with SYS_CAP_COPY, narrowing rights and
// stamping badges as it goes.

enum CapKind { Null, Control, Thread, Endpoint, Notification, Irq, Reply, Space, Frame }

let RIGHT_SEND = 1;            // send / call / signal
let RIGHT_RECV = 2;            // receive / wait
let RIGHT_GRANT = 4;           // copy this cap on; on a thread cap, copy caps into it
let RIGHT_ALL = 7;
// Set only on the cap an object was created into. SYS_CAP_REVOKE needs it,
// and SYS_CAP_COPY always strips it (a move keeps it), so a cap handed to
// someone else, however many rights it keeps, can never destroy the object.
let RIGHT_ORIGINAL = 8;

// obj is the object's slot in its table (tcbs, eps, notifs, spaces, frames)
// or the IRQ line; gen is the generation the cap was minted at. For an
// endpoint cap, badge is what its receiver is told; for a notification, the
// bits a signal sets; for a reply cap, obj and gen name the waiting caller.
struct Cap { kind: CapKind, rights: u8, gen: u32, obj: u32, badge: u64 }
struct CNode { slots: Cap[CNODE_SLOTS] }

var cnodes: CNode[MAX_THREADS];

let NO_CAP = Cap { kind: CapKind.Null, rights: 0, gen: 0, obj: 0, badge: 0 };

fn cnode(t: Tcb&) -> CNode& { cnodes[t.id % MAX_THREADS] }

// t's cap in that slot, if it is of that kind and has every right asked for.
// The generation is checked by the per-object accessor (thread_of, ep_of,
// ...), since only it knows the object's table.
fn lookup(t: Tcb&, slot: u64, kind: CapKind, rights: u8) -> Cap? {
    guard slot < CNODE_SLOTS else { return null; }
    let n .= cnode(t);
    let c .= n.slots[slot];
    guard c.kind == kind && (c.rights & rights) == rights else { return null; }
    return c;
}

fn has_control(t: Tcb&, slot: u64) -> bool {
    let c = lookup(t, slot, CapKind.Control, 0);
    return c != null;
}

// The slot itself, if it is in range and empty.
fn empty_slot(t: Tcb&, slot: u64) -> Cap? {
    guard slot < CNODE_SLOTS else { return null; }
    let n .= cnode(t);
    let c .= n.slots[slot];
    guard c.kind == CapKind.Null else { return null; }
    // A blocked receiver's named reply slot is reserved for the reply cap
    // its next call mints; nothing else may be copied into it meanwhile.
    guard !(t.state == TState.Receiving && slot == t.reply_slot) else { return null; }
    return c;
}

// Whether a slot can take a reply cap: in range, and empty or holding an old
// reply cap (which a new one replaces). Anything else is a live cap that a
// reply must never overwrite.
fn reply_slot_ok(t: Tcb&, slot: u64) -> bool {
    guard slot < CNODE_SLOTS else { return false; }
    let n .= cnode(t);
    let c .= n.slots[slot];
    return c.kind == CapKind.Null || c.kind == CapKind.Reply;
}

fn thread_cap(t: Tcb&) -> Cap {
    Cap { kind: CapKind.Thread, rights: RIGHT_ALL | RIGHT_ORIGINAL, gen: t.gen, obj: t.id,
          badge: 0 }
}

// Thread caps are counted per thread (Tcb.caprefs), so the kernel knows when
// the last one to a not-yet-started thread is gone. Every place a thread cap
// enters a slot calls cap_acquire, every place one leaves calls cap_release
// (a move does neither). A stale cap names no thread and counts for nothing.
fn cap_acquire(c: Cap&) {
    guard c.kind == CapKind.Thread else { return; }
    let t = thread_of(c);
    if t { t.caprefs += 1; }
}

fn cap_release(c: Cap&) {
    guard c.kind == CapKind.Thread else { return; }
    let t = thread_of(c);
    guard t else { return; }
    if t.caprefs > 0 { t.caprefs -= 1; }
    // An Inactive thread is in no queue (linked to itself), so linking it
    // onto the reap queue happens at most once.
    if t.caprefs == 0 && t.state == TState.Inactive && t.next .== t {
        link_before(tcbs[REAP_Q], t);
        reap_pending = true;
    }
}

// A cap leaves its slot: the one place that keeps the slot invariants. A
// thread cap gives back its count; a reply cap can no longer be answered, so
// its caller is released with `status`.
fn drop_cap(s: Cap&, status: u64) {
    if s.kind == CapKind.Reply {
        let caller = thread_of(s);
        if caller { wake_await(caller, status); }
    }
    cap_release(s);
    s = NO_CAP;
}

// The thread a thread or reply cap names, unless it has died since.
fn thread_of(c: Cap&) -> Tcb? {
    let t .= tcbs[c.obj % MAX_THREADS];
    guard c.gen == t.gen else { return null; }
    return t;
}
