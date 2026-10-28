// The user-visible ABI: syscall numbers, status codes and the registers they
// travel in. user/syscall.h is the C side of the same table.
//
// A syscall is the `syscall` instruction with the number in rax and up to
// six arguments in rdi, rsi, rdx, r10, r8, r9. It returns a status in rax
// and a value (a badge, bits, a tick count) in rdi. IPC messages are four
// words in rsi, rdx, r10 and r8, in both directions, and are copied from
// frame to frame without ever touching memory the threads own.

// Register slots, as hal_reg and hal_set_reg number them.
let REG_RAX = 0;
let REG_RDI = 1;
let REG_RSI = 2;
let REG_RDX = 3;
let REG_R10 = 4;
let REG_R8 = 5;
let REG_R9 = 6;
let REG_RIP = 7;               // readable only
let REG_RSP = 8;               // readable only

let SYS_DEBUG_WRITE = 0;       // rdi = address, rsi = length: text to the console
let SYS_YIELD = 1;
let SYS_SEND = 2;              // rdi = endpoint cap; message
let SYS_CALL = 3;              // rdi = endpoint cap; message -> reply message
let SYS_RECV = 4;              // rdi = endpoint cap -> rdi = badge; message
let SYS_REPLY = 5;             // message: to the thread whose call was received
let SYS_REPLY_RECV = 6;        // rdi = endpoint cap; reply message -> as SYS_RECV
let SYS_SIGNAL = 7;            // rdi = notification cap: sets the cap's badge bits
let SYS_WAIT = 8;              // rdi = notification cap -> rdi = bits, now cleared
let SYS_SLEEP = 9;             // rdi = ticks
let SYS_TICKS = 10;            // -> rdi = ticks since boot
let SYS_EXIT = 11;
let SYS_ENDPOINT_NEW = 12;     // rdi = control cap, rsi = empty slot
let SYS_NOTIF_NEW = 13;        // rdi = control cap, rsi = empty slot
let SYS_THREAD_NEW = 14;       // rdi = control cap, rsi = empty slot, rdx = entry,
                               // r10 = stack top, r8 = priority, r9 = argument
let SYS_THREAD_START = 15;     // rdi = thread cap
let SYS_CAP_COPY = 16;         // rdi = thread cap (GRANT) of the receiver, rsi = own slot,
                               // rdx = receiver's empty slot, r10 = rights kept, r8 = badge
let SYS_IRQ_NEW = 17;          // rdi = control cap, rsi = empty slot, rdx = IRQ line
let SYS_IRQ_BIND = 18;         // rdi = IRQ cap, rsi = notification cap (SEND)
let SYS_IRQ_ACK = 19;          // rdi = IRQ cap: the line may fire again

let OK = 0;
let E_CAP = 1;                 // no capability of the kind and rights needed in that slot
let E_SLOT = 2;                // destination slot out of range or not empty
let E_ARG = 3;                 // a bad address, priority or IRQ line
let E_FULL = 4;                // no free kernel object of that kind
let E_STATE = 5;               // the object's state does not allow it, or a reply was dropped
let E_NOCALLER = 6;            // a reply with no call to answer
let E_DEAD = 7;                // the thread a call waited on died before replying
let E_NOSYS = 8;

// Kernel entry vectors, as the arch layer passes them to k_trap.
let VEC_SYSCALL = 0x100;
let IRQ_BASE = 32;
let IRQ_END = IRQ_BASE + NUM_IRQS;
