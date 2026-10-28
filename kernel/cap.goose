// Capabilities. A thread can name a kernel object only through a slot of
// its own capability node, and only use it as far as the slot's rights go.
// Slots are 14-byte values in a fixed table per thread: a lookup is one
// range check on the user's slot number and one load, with nothing to
// allocate or free.
//
// The root thread starts with the only Control cap, which creates objects,
// and hands out what it chooses with SYS_CAP_COPY, narrowing rights and
// stamping badges on the way.

enum CapKind { Null, Control, Thread, Endpoint, Notification, Irq }

let RIGHT_SEND = 1;            // send / call / signal
let RIGHT_RECV = 2;            // receive / wait
let RIGHT_GRANT = 4;           // copy this cap on; on a thread cap, copy caps into it
let RIGHT_ALL = 7;

// obj is the object's index: in tcbs for a thread, in eps or notifs, or the
// IRQ line. For a thread cap, badge holds the thread's generation; for an
// endpoint, the badge its receiver is told; for a notification, the bits
// that signalling sets.
struct Cap { kind: CapKind, rights: u8, obj: u32, badge: u64 }
struct CNode { slots: Cap[CNODE_SLOTS] }

var cnodes: CNode[MAX_THREADS];

let NO_CAP = Cap { kind: CapKind.Null, rights: 0, obj: 0, badge: 0 };

fn cnode(t: Tcb&) -> CNode& { cnodes[t.id % MAX_THREADS] }

// t's cap in that slot, if it is of that kind and has every right asked for.
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
    return c;
}

// The thread a thread cap names, unless that thread has died since.
fn thread_of(c: Cap&) -> Tcb? {
    let t .= tcbs[c.obj];
    guard c.badge == t.gen else { return null; }
    return t;
}

fn thread_cap(t: Tcb&) -> Cap {
    Cap { kind: CapKind.Thread, rights: RIGHT_ALL, obj: t.id, badge: t.gen }
}
