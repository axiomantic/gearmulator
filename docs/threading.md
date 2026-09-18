# The G2 thread map

This document is the MAP. It is prose, and no test reads it — a test that
grepped this file would assert the prose, pass while the code was wrong, and go
red when someone edited a sentence. The behaviour the map describes is asserted
by `t0_thread_map`, through `Scheduler::owningThread()`, which is present in
every build type.

## The two threads

There are exactly two, and ownership moves between them exactly once.

**The boot thread** builds the machine, downloads the firmware, runs the boot
quanta and performs the hand-off. It is the thread that calls
`Scheduler::create`.

**The audio thread** is the host's callback thread. It owns the object from its
first `runFrames` after the hand-off until the object is destroyed or `reset`.

**The hand-off is `beginPlayPhase`, and it is the boot thread's last Scheduler
action.** It clears the recorded owning-thread identity as its final act, so
that the first `runFrames` of the play phase re-establishes the owner on the
audio thread instead of tripping on the boot thread's identity.

`reset()` returns the object to the pre-hand-off state, so the cycle may begin
again. It is the boot thread's.

## How ownership is recorded, and how it is checked

`Scheduler::runFrames` records the calling thread on its first call after each
clearing. A default-constructed `std::thread::id` means NO OWNER IS RECORDED,
which is the state a fresh `Scheduler` starts in and the state
`beginPlayPhase` and `reset` leave.

A **debug** build additionally asserts, in `runFrames`, that a caller which is
not the recorded owner is a defect. **That assertion is the predicate of
nothing.** A release build defines `NDEBUG` and removes it, and the thread map
is the property that stops a data race in the SHIPPED build — so the recorded
identity is EXPOSED through `owningThread()` and `t0_thread_map` reads it there,
in whichever build type the tree is configured for.

## `Scheduler` — 12 rows over 20 methods

| Row | Methods | Owning thread |
|---|---|---|
| 1 | `create` | boot |
| 2 | `reset` | boot |
| 3 | `beginPlayPhase` | boot — and it is the boot thread's LAST action |
| 4 | `runFrames` | boot before the hand-off, audio after it. THE ONE ROW THAT NAMES BOTH |
| 5 | `push` | audio |
| 6 | `pull` | audio |
| 7 | `frameIndex` | audio |
| 8 | `underrunFrames`, `secondBusUnderrunFrames`, `phaseErrorFrames` | audio |
| 9 | `starvedFrames`, `overflowFrames`, `droppedFrames`, `underflowFrames` | audio |
| 10 | `cycleDebt`, `longDispatchQuanta` | audio |
| 11 | `faulted`, `contextFaulted`, `contextFault` | audio — read AFTER `runFrames` returns |
| 12 | `owningThread` | any thread. It reads one `std::thread::id` and is the only accessor a non-owning thread may call, because it is what a non-owning thread calls to find out whether it may call the others |

Rows 1 to 4 are the boot thread's; rows 5 to 11 are the audio thread's; row 12
is neither. 1 + 1 + 1 + 1 + 1 + 1 + 1 + 3 + 4 + 2 + 3 + 1 = 20.

**A console harness that wants an observability accessor from another thread
reads a copy the audio thread published, never the accessor.** Rows 7 to 11 are
`const` and read a single scalar each, which makes them look safe from any
thread and is exactly why the rule is written down: a torn or stale read of a
counter is invisible, and the counters are how this project measures itself.

### The counts, and the methods this table does not cover

**This table is 12 rows over 20 methods, measured by listing the public
declarations of `class Scheduler` in `g2Lib/scheduler.h`.** Four further methods
belong to the class's surface elsewhere in the project and are not covered here:

* `stateSize`, `stateSave`, `stateLoad` — the state trio. They land with the
  state work, not with the thread map.
* `queueMidi` — declared nowhere in `g2Lib`.

Adding those four as two further rows — one for the state trio, one for
`queueMidi` — would give 14 rows over 24 methods. **The table is written from
the header rather than from any planned surface, because a row for a method that
does not exist documents fiction.** When the state trio lands, its writer adds
the state-trio row: `stateLoad` is the boot thread's, and `stateSize` and
`stateSave` go with it.

## `TransportHub` — 2 rows over 6 methods

| Row | Methods | Owning thread |
|---|---|---|
| 1 | the constructor, `attach`, `detach` | boot. The allocation is fixed at construction and this project forbids allocating after it, so the endpoint set is built before the audio thread exists |
| 2 | `fromDevice`, `toDevice`, `drainToDevice` | audio, inside a quantum boundary. `toDevice` is called from an endpoint's own thread with a BORROWED buffer, which is why the hub copies |

**`g2Lib/transportHub.h` declares six public members, and the table above covers
all six** — the constructor, `attach` and `detach` in row 1; `fromDevice`,
`toDevice` and `drainToDevice` in row 2.

## The borrow lifetime, which is a threading rule wearing another name

The pointers one `drainToDevice` returns are hub-owned and stay readable until
the NEXT `drainToDevice` on the same hub. Because both calls are row 2's, that
window is the audio thread's alone, and no other thread may hold one of those
pointers across it.
