// Sizes and policy. Untyped `let`s are named constants: each adapts to the
// integer type it meets, as a literal does, and can size an array.

let MAX_THREADS = 64;          // a power of two, and the same in arch/x86_64/hal.h
let NUM_PRIOS = 64;            // one bit each in the u64 ready mask
let MAX_ENDPOINTS = 32;
let MAX_NOTIFS = 32;
let MAX_SPACES = 16;           // address spaces
let MAX_FRAMES = 128;          // frame objects (each a 4 KiB page)
let CNODE_SLOTS = 16;          // capability slots per thread
let NUM_IRQS = 16;             // the two 8259s
let TIMESLICE = 2;             // ticks per slice: 20 ms at the arch layer's 100 Hz

let IDLE = 0;                  // thread 0 is the idle thread
let IDLE_PRIO = 0;             // and the only one at priority 0
let ROOT_PRIO = 50;

// tcbs holds the threads first, then the sentinels of every queue a thread
// can wait in. A queue is a circular list through its sentinel, so linking
// and unlinking never meet an empty-list special case.
let RQ_BASE = MAX_THREADS;             // tcbs[RQ_BASE + p]: priority p's run queue
let FREE_Q = MAX_THREADS + NUM_PRIOS;  // thread slots not in use
let SLEEP_Q = FREE_Q + 1;              // sleeping threads, soonest first
let REAP_Q = SLEEP_Q + 1;              // unstarted threads whose last cap is gone
// Endpoint and notification queues are pushed after these as objects are made.
