// The scheduler: strict priorities, round robin within one, O(1) to pick,
// block or wake a thread.
//
// Each priority has a run queue; bit p of ready_mask says queue p is not
// empty. The next thread is the first of the highest non-empty queue, found
// with one count-leading-zeros. The running thread stays at the front of
// its queue while it runs; using up its slice or yielding moves it to the
// back. The idle thread is alone at priority 0 and never blocks, so the
// mask is never empty.

var ready_mask: u64 = 0;
var ticks: u64 = 0;

fn rq(prio: u8) -> Tcb& {
    let p: i64 = prio;
    return tcbs[RQ_BASE + p];
}

fn prio_bit(prio: u8) -> u64 {
    let one: u64 = 1;
    return one << prio;
}

// t becomes runnable behind the threads already ready at its priority.
fn make_ready(t: Tcb&) {
    t.state = TState.Ready;
    link_before(rq(t.prio), t);
    ready_mask |= prio_bit(t.prio);
}

// The same, in front of them: for a thread handed the CPU by one that
// blocks to wait for it (an IPC call or reply), so a round trip does not
// wait behind unrelated threads of the same priority.
fn make_ready_first(t: Tcb&) {
    t.state = TState.Ready;
    link_after(rq(t.prio), t);
    ready_mask |= prio_bit(t.prio);
}

// Takes a Ready thread off its run queue; the caller gives it its new state.
fn make_unready(t: Tcb&) {
    unlink(t);
    if is_empty(rq(t.prio)) { ready_mask &= ~prio_bit(t.prio); }
}

// To the back of its queue, with a fresh slice.
fn rotate(t: Tcb&) {
    let q .= rq(t.prio);
    unlink(t);
    link_before(q, t);
    t.slice = TIMESLICE;
}

fn pick() -> Tcb& {
    let top = 63 - hal_clz(ready_mask);
    let q .= tcbs[RQ_BASE + top];
    return q.next;
}

// The sleep queue is kept sorted, so a tick only ever looks at its front.
fn sleep_until(t: Tcb&, when: u64) {
    t.state = TState.Sleeping;
    t.wake_at = when;
    let s .= tcbs[SLEEP_Q];
    var pos .= s.next;
    while pos .!= s && pos.wake_at <= when { pos .= pos.next; }
    link_before(pos, t);
}

fn tick() {
    ticks += 1;
    let s .= tcbs[SLEEP_Q];
    loop {
        let t .= s.next;
        if t .== s || t.wake_at > ticks { break; }
        unlink(t);
        make_ready(t);
    }
    let c .= cur();
    if c.state == TState.Ready && c.id != IDLE {
        if c.slice > 1 { c.slice -= 1; } else { rotate(c); }
    }
}
