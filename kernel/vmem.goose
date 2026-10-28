// Address spaces and frames, the policy over the arch layer's page tables.
//
// A Space wraps the physical PML4 that hal_vspace_new built (sharing the
// kernel's low map, with a private 512 GiB window for user pages). A Frame
// wraps one physical page from the pool. The core hands both out behind
// capabilities and remembers, per frame, where it is mapped, so destroying
// a frame or a space unmaps cleanly. Both tables carry a generation and are
// reused when destroyed, so a stale Space or Frame cap misses.

struct Space { pml4: u64, gen: u32, live: bool, refs: u32 }
struct Frame { phys: u64, gen: u32, live: bool, mapped: bool, space: u32, vaddr: u64 }

var spaces: Space[>..] = [];
var frames: Frame[>..] = [];

fn new_space() -> i64 {
    let pml4 = hal_vspace_new();
    guard pml4 != 0 else { return -1; }
    for i in spaces.len {
        let s .= spaces[i];
        if !s.live { s.pml4 = pml4; s.live = true; s.refs = 0; return i; }
    }
    guard spaces.len < MAX_SPACES else { hal_vspace_free(pml4); return -1; }
    spaces.push(Space { pml4: pml4, gen: 0, live: true, refs: 0 });
    return spaces.len - 1;
}

fn new_frame() -> i64 {
    let phys = hal_frame_alloc();
    guard phys != 0 else { return -1; }
    for i in frames.len {
        let f .= frames[i];
        if !f.live { f.phys = phys; f.live = true; f.mapped = false; f.space = 0; f.vaddr = 0;
                     return i; }
    }
    guard frames.len < MAX_FRAMES else { hal_frame_free(phys); return -1; }
    frames.push(Frame { phys: phys, gen: 0, live: true, mapped: false, space: 0, vaddr: 0 });
    return frames.len - 1;
}

fn space_of(c: Cap&) -> Space? {
    guard c.obj < spaces.len else { return null; }
    let s .= spaces[c.obj];
    guard s.live && s.gen == c.gen else { return null; }
    return s;
}

fn frame_of(c: Cap&) -> Frame? {
    guard c.obj < frames.len else { return null; }
    let f .= frames[c.obj];
    guard f.live && f.gen == c.gen else { return null; }
    return f;
}

// Maps a frame into a space at vaddr. A frame is mapped in one place at a
// time, so a second map needs an unmap first.
fn map_frame(s: Space&, si: u32, f: Frame&, vaddr: u64, writable: bool) -> u64 {
    guard vaddr >= USER_VBASE && vaddr < USER_VTOP else { return E_ARG; }
    guard !f.mapped else { return E_STATE; }
    guard hal_map(s.pml4, vaddr, f.phys, writable) else { return E_ARG; }
    f.mapped = true;
    f.space = si + 1;
    f.vaddr = vaddr;
    return OK;
}

fn unmap_frame(f: Frame&) -> u64 {
    guard f.mapped && f.space != 0 && f.space <= spaces.len else { return E_STATE; }
    let s .= spaces[f.space - 1];
    hal_unmap(s.pml4, f.vaddr);
    f.mapped = false;
    f.space = 0;
    return OK;
}

// Binds a not-yet-started thread to an address space, so THREAD_START runs
// it there. A thread is in one space for its life.
fn bind_space(t: Tcb&, s: Space&, si: u32) {
    t.space = si + 1;
    s.refs += 1;
    hal_set_space(t.id, s.pml4);
}

// A thread ending gives back its hold on its space.
fn release_space(t: Tcb&) {
    if t.space != 0 && t.space <= spaces.len {
        let s .= spaces[t.space - 1];
        if s.refs > 0 { s.refs -= 1; }
    }
}

fn destroy_frame(i: u32) {
    guard i < frames.len else { return; }
    let f .= frames[i];
    guard f.live else { return; }          // never free a physical frame twice
    if f.mapped { unmap_frame(f); }
    hal_frame_free(f.phys);
    f.live = false;
    f.gen += 1;
}

// A space can be destroyed only when no live thread runs in it. Frames
// mapped into it keep their objects but lose their mapping, since the
// page tables go with the space.
fn destroy_space(i: u32) -> bool {
    guard i < spaces.len else { return false; }
    let s .= spaces[i];
    guard s.live else { return false; }    // never free a PML4 twice
    guard s.refs == 0 else { return false; }
    for j in frames.len {
        let f .= frames[j];
        if f.live && f.mapped && f.space == i + 1 { f.mapped = false; f.space = 0; }
    }
    hal_vspace_free(s.pml4);
    s.live = false;
    s.gen += 1;
    return true;
}
