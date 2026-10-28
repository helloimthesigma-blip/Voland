# Parallel guest threads

Status: opt-in scheduler mode, the default in the web build (three cores
where the machine has room).
This document is the design; see "Measuring" for how to measure it.

## Why

Every guest thread used to run as a green thread on one host thread
(DESIGN.md §7). Silksong keeps two guest threads busy, the main thread
(53% of cycles to the title) and thread 32 (36%). Running them on two
host threads at once is worth up to about 1.8×, and the gain multiplies
with the JIT.

## Shape: a driver and N cores

`emulator_set_host_cores(emu, n)`:

- `n = 0`, the default for the CLI and tests, keeps the serial scheduler
  exactly as it was.
- `n >= 1` starts `n` **core threads**. The calling thread becomes the
  **driver**: the CPU worker on the web, the CLI's main thread natively.

Each `emulator_run_slice` is one **slice**:

1. The driver takes the kernel lock and runs the device updates (`vi`,
   `hid`, audio, `nvdrv_poll_completions`), as in serial mode.
2. The driver runs the serial scheduler's preamble: exited/crashed
   checks, `expire_timeouts`, and whether anything is runnable. If nothing
   is, `scheduler_idle` jumps time exactly as in serial mode and the slice
   ends without waking any core.
3. Otherwise the driver opens the slice (a generation counter plus a
   condvar broadcast) and waits.
4. Each core loops, under the lock:
   1. Pick the best runnable thread that is not `on_core`.
   2. Mark it, release the lock, and call `backend->run(state, budget)`
      unlocked.
   3. Retake the lock and account the run (`scheduler_finish_run`).

   A core keeps picking only while **another core is still running** and
   its own cycles in this slice are below the budget. With nothing to
   pick it waits on the condvar for a run to end or a thread to wake.
5. The slice ends when every core has stopped. The driver returns.

Guest code runs only inside `emulator_run_slice`. Between slices, every
API (loading, snapshots, input, frame dumps, `emulator_step`) sees a
quiescent machine, just as in serial mode.

### One core equals serial

With one core there is never "another core still running", so the core
makes exactly one run per slice: the serial scheduler's pick, budget,
CNTVCT and time accounting. `tests/parallel_test.c` checks this on every
guest program at five budgets (slices, SVCs, virtual time and output
identical). Silksong to 900k slices gives the same virtual time, SVC count
and thread table (see the status file).

From two cores on, the run is no longer deterministic.

### Why the driver does not run guest code itself

On the web, two host hooks only work on the CPU worker's own thread:

- reading the game file (`FileReaderSync` over the `File`);
- guest debug output (`Module.volandGuestOutput`).

A core inside an SVC that needs one calls `parallel_on_driver(fn, ctx)`.
The request goes through a small channel (its own mutex and condvar, so it
never needs the kernel lock, which the requesting core holds). The driver
serves it while it waits for the slice. A driver busy running guest code
could not serve it promptly.

Natively the hooks are thread-safe, and the channel is exercised only by
the test.

## The kernel lock

One mutex guards the scheduler and all of HLE:

- **Cores** hold it to pick and to account runs.
- **Every SVC** runs under it: `hle_on_svc` brackets the dispatch with
  `scheduler_kernel_enter` / `scheduler_kernel_exit`, which are no-ops in
  serial mode.
- **`enter`** points `sched->current` at the calling core's thread, so
  `scheduler_current()` keeps meaning "the thread making this SVC" for
  all of HLE unchanged.
- **`exit`** broadcasts to the cores, because the SVC may have woken
  threads.

This covers the GPU producer (raster3d is reached through nvdrv ioctls),
IPC and services, and the vmm mapping SVCs. Device updates run in the
driver before the slice opens, when no core is running.

### Threads leaving a core

- **`on_core`.** `Sched_Thread.on_core` marks a thread inside `run()` on
  some core. `scheduler_pick` skips it.
  - A thread that blocks in an SVC is WAITING while its core finishes
    `run()`.
  - Another core may wake it meanwhile (the wake writes X0/X1 of a
    register file no one is writing), but cannot pick it until it is off
    its core.
  - Backends must not write guest registers after the SVC handler
    returns. The interpreter doesn't.
- **Reclaiming exited threads** (`reclaim_threads` in `svc_thread.c`)
  skips `on_core` threads: an exited thread's CPU state may still be
  leaving `run()`.
- **SetThreadActivity(Paused)** on a thread running on another core
  blocks the caller (`WAIT_OFF_CORE`) until that thread leaves its core,
  as Horizon does.
  - Silksong's GC pauses threads this way and then reads them with
    GetThreadContext3 (64 and 14 calls to the title). Without the wait it
    would read a register file that is still changing.
  - The paused thread finishes at most its current run (one budget).
  - In serial mode a thread making an SVC is never `on_core` elsewhere,
    so this never blocks there.

### GPU work

A GPFIFO submission (nvdrv SUBMIT_GPFIFO / KICKOFF_PB) runs the GPU command
processor: draw recording, vertex fetch and compute programs on the CPU. In
an SSBU match that is most of a frame, and under the kernel lock it kept
the other cores waiting 45–73% of the time. So it runs outside it.

- **The GPU lock** (`scheduler_gpu_lock`/`unlock`, `Parallel.gpu`)
  serialises everything that reads or changes what command processing
  uses: the command processor and renderer, nvmap handles, GPU mappings
  (nvmap and address-space ioctls), the multimedia engines (they write
  into the renderer's textures), and VI's composite and frame end.
- **Order:** the GPU lock is only ever taken with the kernel lock held,
  never the other way round. `scheduler_gpu_begin` takes the GPU lock and
  then releases the kernel lock. `scheduler_gpu_end` releases the GPU lock
  and only then takes the kernel lock back. So a holder of the kernel lock
  waiting for the GPU lock never waits on a thread that wants the kernel lock.
- **`current`:** `gpu_begin` gives `sched->current` back to its value outside
  the SVC, and `gpu_end` restores the SVC's thread.
- **While the submission runs:**
  - Other cores' IPC must not touch the suspended call's state: the IPC
    request and response (`svc_ipc.c`) are per host thread. They were one
    static pair, and SSBU crashed at boot on 3 cores (a reply overwritten
    mid-call, then a jump to address 0).
  - Other cores' ioctls may reuse nvdrv's shared ioctl buffer. The request's
    bytes are saved in a thread-local buffer and put back before the reply.
  - In-stream syncpoint increments are queued and applied once the kernel
    lock is back, before the submission's own fence completes.
- **Serial mode:** all of these are no-ops.
- **Test:** `parallel_test` checks that another thread's SVC gets in
  between `gpu_begin` and `gpu_end`, and that `current` comes back.

### Guest synchronization words

The kernel lock serializes HLE, not guest code. A mutex word, condvar key
or address-arbiter counter is also changed by guest LDXR/STXR on other
cores while an SVC handles it. Two rules in `svc_thread.c` follow:

- **Every kernel update of such a word is an atomic compare-and-swap**
  (`vmm_compare_exchange32`), never a read and then a write. Example: a
  SignalProcessWideKey that read "owned" and then wrote "owned + waiters"
  could overwrite the owner's own release made in between, leaving a
  mutex owned by nobody's knowledge (a deadlock).
- **WaitProcessWideKeyAtomic marks the key before it releases the
  mutex.** Signallers (libnx, nn::os) take the mutex, then skip the SVC
  while the key reads 0. Released first, a signaller on another core can
  slip in between and lose the wakeup.
  - A two-core gameplay run once stalled with the main thread asleep on a
    condvar. This ordering and the read-then-write above are the races
    that can cause that.
  - The race window is a few host instructions, too narrow for the stress
    test to hit reliably. The test (condvar.s with key-checking
    signallers, 20 rounds on 2 and 3 cores) guards the path. The evidence
    is repeated Silksong runs (status file).

Each swap is seq_cst, so the kernel's updates are also ordered as other
cores see them. With one core every swap succeeds first time, exactly
like the plain write it replaced.

## Virtual time

`scheduler.ticks` stays the one global clock (19.2 MHz, backing CNTVCT,
GetSystemTick and every timeout).

- **At the start of a run** the run starts at the current time; that
  value goes to CNTVCT.
- **At the end of a run**, time becomes `max(time, start + cycles)`.
  Concurrent runs therefore overlap in virtual time instead of adding up:
  two cores each running 200k cycles advance the clock by 200k cycles'
  worth, not 400k.
  - When no other core moved time during the run (always so with one
    core), the serial update is used verbatim, including the carried
    sub-tick remainder. That is what keeps one core bit-identical.
- **SVC handlers** see the global time, as in serial mode, where they see
  the time at the start of the run.

Consequence for pacing: per virtual second, the game now gets one virtual
CPU per core instead of one virtual CPU shared by all threads. A title
bound by two busy threads runs its frames in less virtual time, and
virtual time advances with the slowest-progressing host core rather than
the sum of all of them. Vsync stays 60 Hz of virtual time.

### Wall-clock pacing

Virtual time advances with guest work. When the host is slower than the
Switch, the game therefore runs in slow motion: Silksong renders 60 fps of
virtual time at ~16 per wall second, so its world moves at ~27% speed.
`emulator_set_pacing` (on by default in the web, `?pacing=0` opts out)
makes virtual time keep up with wall time.

- **Where it acts.** Before each slice, and at the start of each
  free-running burst, when no guest code runs.
- **What it does.** Virtual time that has fallen behind the wall clock
  (since the pacing origin) jumps forward, as an idle jump does. Due
  timers and device events (vsync, audio) then fire at the new time. The
  guest sees large frame deltas and runs at real speed with fewer
  rendered frames.
- **Cap.** One jump is at most 100 ms (`EMULATOR_PACING_MAX_JUMP_MS`).
  Further behind - a hidden tab, a GC pause - the origin resyncs, so the
  game never skips a big chunk at once.
- **Resync.** The origin also resyncs at run start and on resume, so
  paused time does not count.
- **Waiting is paced too.** Idle jumps, when every guest thread waits, stop
  at the wall clock plus a 4 ms lead (`Scheduler.time_limit`); the slice
  then returns idle until wall time catches up.
  - Without this, waiting raced ahead of wall time (1.5× in Silksong). The
    guest's timeouts on host-side events, such as GPU fences signalled by
    the GPU worker in wall time, expired before the events could arrive.
    The game stopped presenting (0 fps) while virtual time kept running.
  - With nothing to wake on its own, a paced run waits rather than
    reporting a deadlock, since a host-side event may still come.
  - Guest code that runs ahead of wall time is not slowed down; virtual
    time only moves forward and stays monotonic.
- **Deterministic runs.** The core default, the CLI and the tests stay
  unpaced, so they remain deterministic and the goldens are unchanged.

## Free-running mode (the web default)

The slice mode above pays a handoff on every slice: about 110 us to wake
the cores and 50 us for the driver to notice the end, at ~1,100 slices/s
in Silksong gameplay - about 18% of host time, the cores' "park" share.
Free-running mode removes the per-slice barrier and keeps everything else.
The browser runs free from the start (`?free=0` for slices; `?free=N`
from slice N on), except titles in the CPU worker's `SLICED_TITLES`, which
run in slices unless `?free=` says otherwise: Tears of the Kingdom, at
10.1 fps sliced vs 8.7 free at the same virtual-time rate, with its
multicore loader crash 1 run in 12 vs about 1 in 4. The slice mode stays the default for the CLI, the tests,
the goldens and `tools/perf.mjs` recipes, which count slices (it is
deterministic).

### Shape

- **One call per burst.** `emulator_run_for(emu, host_ms, budget)`
  replaces many `emulator_run_slice` calls. The web CPU worker makes one
  call per ~12 ms burst.
- **Inside the call.**
  - The cores run continuously: pick, run up to `budget` cycles,
    account, pick again. There is no slice and no barrier.
  - The driver only serves host calls (`parallel_on_driver`) and waits
    for the host deadline or for the process to stop.
- **Before the call returns** the driver pauses the cores: each finishes
  its current run, at most one budget, and parks. So guest code still
  runs only inside the call, and loads, snapshots, input and settings
  between calls see a quiescent machine.
  - That is one handoff per burst instead of one per slice: about
    160 us per 12 ms, ~1.3%.

### Devices and time

- **Device updates run on a core,** under the kernel lock, whenever they
  are due:
  - `nvdrv_poll_completions`, `hid_update`, `vi_update` (vsync,
    composite, GPU present), `audout`/`audren`;
  - then `device_wake_at` is recomputed (`emulator_update_devices`, the
    serial run loop's preamble factored out).
  - They are plain core-side C with no JS hooks.
- **When updates are due:**
  - after any run that leaves `scheduler.ticks >= device_wake_at`;
  - when a core finds nothing runnable while no core is busy.
- **That second case is the idle path.**
  - The core does the serial scheduler's idle step: jump time to the
    earliest wake or device event.
  - It then runs the device updates, and the cores continue.
  - DEADLOCK and EXITED stop the call, which returns that status.
- **Virtual time:** the same rule as slice mode. A run starts at the global
  time and ends at `max(time, start + cycles)`. Timeouts expire before
  every pick. A device event is handled by the first core to see time
  pass it, so it lands at most one run (≤ budget cycles) late in virtual
  time, as in slice mode, where it waits for the next slice.
- **The pause takes the kernel lock while serving host calls.** A core
  can be inside an SVC, holding the kernel lock, while it waits for the
  driver to serve a host call (on the web, every game-file read). So the
  driver tries the lock and serves pending calls until it gets it; a plain
  lock deadlocked the browser at boot, where reads are frequent.
- **Everything else is unchanged:** poll coalescing, its revert-before-pick
  and work epoch, the exclusive monitor, SVCs under the kernel lock, and
  affinity.

### Status

`emulator_set_free_running`; on by default in the browser since
2026-10-22, the CLI's `--free-running-from N` opts in. What was asked of it
before it became the default:

- `parallel_test` on 2–3 cores, including polls and the
  exclusive-monitor stress, 20+ rounds;
- a long browser gameplay session with no gaps in presents or ticks;
- an interleaved browser A/B showing a clear win.

Measured 2026-10-22 (Silksong, 3 cores, `?pacing=0`, 30 s): gameplay from
a save state 7.92 M ticks/s, 24.7 fps sliced vs 9.49 M, 29.6 fps free
(+20%); the title load from 8 s virtual 7.92 M, 21.1 fps vs 8.75 M,
24.0 fps (+10%). From boot to 20 s virtual, then 30 s: Hollow Knight
40.5 -> 48.3 fps, Nine Sols 19.3 -> 24.4, Celeste 29.5 -> 30.4, SSBU
54.4 -> 53.7 (noise); an MK8 race 23.4 both, its multicore tiles no
worse. 60 s of Silksong at the default pacing: steady 40-45 fps, no gaps.
Save states load across sessions in either mode.

## Pinned cores (opt-in)

`emulator_set_pin_ideal_cores` (web `?pincores=1`, CLI
`--pin-ideal-cores`) runs every thread only on the host core of its ideal
(preferred) guest core, when its affinity mask allows that core; a thread
whose ideal core is outside its mask keeps the mask's choice. Threads that
share an ideal core then never run at once, wherever their masks would
let them float.

What it changes, measured 2026-10-22 (small samples):

- **Tears of the Kingdom**, native interpreter, three cores to 400k slices
  (~40 s): unpinned, 3 of 3 runs failed (a fault, a guest exit, a stall;
  the same faults recur: main+0x70b6bc reading 0x15c, an sdk routine);
  pinned, 3 of 4 runs got through (one to 1.6M slices), one faulted.
- **Mario Kart 8 Deluxe** on three cores: no help. Its black tiles at the
  start of a race show up pinned too (one pinned run without them was
  slowed by another job on the machine: they look timing-dependent), and
  the race runs at the same ~23-24 fps either way.
- Keeping a thread on its core only while it runs (not moving it after a
  run that ended at the budget or in a non-blocking SVC) did not help
  Tears of the Kingdom: 3 of 4 runs faulted.

It stays off by default, a tool for chasing the open multicore bug.

## Free placement (opt-in)

`emulator_set_free_placement` (web `?placement=free`, CLI
`--free-placement`; `FREE_PLACEMENT_TITLES` in `cpu.worker.ts` for a
per-title default, empty for now) lets any host core run any thread. Affinity
is still reported: GetCurrentProcessorNumber answers the guest core that
the thread's mask and the host core it runs on give, as without it. But
it is not enforced, so threads pinned to one guest core may run at once.
That is not the Switch's rule. It is for titles that pile their threads
onto one guest core, which a host core cannot carry at the Switch's
speed.

Tears of the Kingdom, native interpreter, three cores, 300k slices
(2026-10-24):

| Placement | Presents in 300k slices (survivors) | Runs crashing ~13-16 s in |
|---|---|---|
| Serial | ~60 | 0 of 6 (also clean through 1.2M slices / 787 presents) |
| Affinity enforced (default) | ~410 | ~9 of 12 |
| Free | ~2930 (~24 per virtual second) | 3 of 9 |

## Diagnostics for the open multicore crash

- `VOLAND_SOLO_THREADS=0x18802c,...` (thread handles, stable from run to
  run) or `=all`: those threads run alone - nothing else is picked while
  one runs, and one is picked only when no core is busy.
- `VOLAND_BUDGET_JITTER=SEED`: each slice's budget scaled by a
  deterministic pseudo-random 1/2..3/2, for other interleavings in serial
  mode.

What they showed for Tears of the Kingdom (2026-10-24; crashes in its
resource loader - a zstd-style decoder reading a corrupt context, a
`BinaryFileHeader` that claims "relocated" over raw offsets - or a
game-initiated ExitProcess):

- `all` (multicore scheduling, no two threads at once) still crashes,
  later (196k-271k slices), with serial exclusive semantics as well. Serial
  with jitter never did. So the crash needs the multicore scheduler's
  *order* - a lower-priority thread on one core running while a
  higher-priority thread of another core waits, which serial never does -
  not two threads touching memory at once.
- Solo for the thread doing Map/UnmapPhysicalMemory (" Memory"): the early
  crashes went away (2 of 6 crashed, both late). Never reusing released
  physical pages: no change.
- Floating guest cores (any host core, one thread per guest core at a
  time): 3 of 3 crashed early, so the static guest-to-host mapping is not
  the cause either.

**Deterministic repro (serial):** `VOLAND_SERIAL_ROTATE=3` makes the serial
scheduler take guest cores 0-2 in turn (a slice each, that core's best
thread) - the order several host cores give. Tears of the Kingdom then
crashes every time, identically: slice 443225, tick 467396835,
cDecompresionThread (h170029) at main+0x98c5c, its bitstream pointer 4 GiB
low (a bits-consumed count gone wild, `asr w3, w7, #3` zero-extended). The
same run without rotation is clean through 1.2M slices.

What the deterministic run showed (tools below):

- The input is intact. The decoder reads TOTK's `MCPK` (mesh codec pack)
  read by "Loading 0" (h150025) at tick 466216396: romfs 0xf17b3920+0x6eea0
  into 0x1014394d68. It has the same hash in the normal run, and at the
  crash the buffer still hashes the same.
- From 466430329, when h170029 starts reading that section, to the crash,
  no other thread loads or stores in its context and workspace
  (0x1013f84000+0xb0000) or its stack.
- Normal serial decodes everything through one decoder context
  (0x1013f84df8, h170029's). Rotated, a second context (0x1013d04df8) is
  used too, and h150025 runs 259 slices interleaved with h170029's
  decode.
- Open: whether the section bounds h170029 gets are already wrong (from
  the pack's parse), or shared state outside the watched ranges is.

Tools: `VOLAND_RUN_TRACE=FROM:TO` (every serial run: thread, start,
cycles, exit, pc); `VOLAND_READ_DELAY_US=N` (voland-cli: each game-file
read takes N us of host time, like an uncached browser read - 4 Node
Tears of the Kingdom runs at 3000 did not crash); `VOLAND_CORE_STALL=ONE_IN:US`
(after a run, one time in ONE_IN the core's host thread sleeps US us with
its guest thread on the core, as when the host OS takes the core away -
4 such TOTK runs at 200:8000 did not crash either); `VOLAND_FS_HASH=1` (FNV-1a of every IStorage read);
`VOLAND_WATCH_GUEST=GVA:BYTES` with `VOLAND_WATCH_GUEST_FROM=TICKS` and
`VOLAND_WATCH_GUEST_SKIP=HANDLE` (guest loads and stores there, with thread
and tick); `VOLAND_REG_WATCH=LO:HI[:TLS]` with `VOLAND_NO_PREDECODE=1`
(instructions that move a register into a range);
`VOLAND_PC_LOG=PC[,PC...]` (x0-x3, x8, x19-x21, x29, x30, and the words at `[x1]`,
`[[x1]+0x40]` and `[x8+0x40]`, each time one of those PCs runs - a queue
push and the item it hands over; every engine: the JIT leaves through the
interpreter at those PCs) with `VOLAND_PC_LOG_WATCH=OFF` (each logged PC
also watches the 8 bytes at `[x1]+OFF` - loads and stores with pc, lr and
thread; the JIT's stores take the helper path while it is set, so
compiled code is watched too) and `VOLAND_PC_LOG_DUMP=PC` (one of the
logged PCs: voland-cli dumps every thread's state there - on core, held,
waiting and on what, last core) and `VOLAND_PC_LOG_DEREF=REG:OFF` (also
the pointer at `[xREG+OFF]` and the 16 bytes it points to - a stream
position and the data there) and `VOLAND_PC_LOG_FRAMES=N` (up to N return
addresses from the x29 frame chain: a call stack per hit). In the browser these variables come from
`?env=NAME=VALUE;NAME=VALUE` (set before the game loads), with `?aslr=0`
so modules sit where voland-cli puts them and the PCs mean the same;
`VOLAND_TRACE_CALLS` with `VOLAND_TRACE_MATCH_X1=OFF:VALUE` and
`VOLAND_TRACE_DUMP_MEM=GVA:BYTES:FILE` (memory at a matching call).

## Memory model

`cpu_set_multicore(true)` is set when two or more cores are on. With one
core nothing changes.

- **Store-exclusive is a host compare-and-swap** against the bytes the
  load-exclusive read (`Interp_State.exclusive_value`). It succeeds only
  if the monitor is set on the same address and size and memory still
  holds those bytes.
  - A store by another host thread in between makes it fail, never
    lost.
  - ABA: a bare CAS would pass when another core changed the bytes and
    changed them back (a lock-free stack's pop X, pop Y, push X), which
    a hardware monitor never allows. So each 64-byte granule hashes to
    a sequence word (`interp_exclusive_seq`). The load-exclusive reads it
    *before* the data (a seqlock read: read after, two complete updates
    in between would pair the old bytes with the new sequence). The
    store-exclusive claims it (a lock bit), does the CAS, and bumps it
    on success. Any successful exclusive store to the granule in between
    therefore fails it. Plain stores don't bump the sequence, so a
    plain-store-only ABA can still pass.
  - 1, 2, 4 and 8 bytes use `__atomic_compare_exchange_n`.
  - The 16-byte pair (LDXP/STXP of two X registers) uses the native
    128-bit CAS on AArch64 hosts.
  - On wasm, which has no 16-byte atomics, the pair is serialized by a
    spinlock. A plain store racing into those 16 bytes can still be lost
    there. It is rare and documented here.
- **Barriers.** DMB and DSB are full host fences (`__atomic_thread_fence`,
  seq_cst). ISB stays a no-op.
- **Acquire/release.** LDAR and STLR get a full fence before and after
  the access (RCsc: an STLR then LDAR pair is not reordered). LDAXR gets
  an acquire fence; STLXR's CAS is already seq_cst.
- **Translation caches.** `vmm_unmap` and a `vmm_reprotect` that takes a
  permission away bump the translation generation before *and after*
  writing the page-table entries, and multicore JIT code reads it with an atomic
  (acquire) load at region entry. In the other order a core could walk
  the table between the bump and the clear and keep the old translation
  under the new generation - reading or writing a page that was freed
  and handed to another allocation. A region already running keeps its entries
  until it exits, as a core between two TLB shootdown points would.
- **Plain loads and stores** stay plain. Aligned accesses of up to 8 bytes
  are single-copy atomic on both hosts (AArch64, and wasm's aligned
  loads/stores in practice).
- **AArch32** (`CPU_BACKEND_A32`, `CPU_BACKEND_A32_JIT`) follows the same
  rules: STREX/STLEX of 1, 2, 4 and 8 bytes is the compare-and-swap,
  exclusives are aligned (an alignment fault otherwise), and DMB/DSB,
  CP15DMB/CP15DSB, LDA/STL and LDAEX fence. The A32 JIT compiles
  exclusives only for one core; with several it calls the interpreter
  for them. `tests/a32_parallel_test.c` (and `a32_parallel_test_node` on
  the JIT) runs three host threads of LDREX/STREX and LDAEX/STLEX
  increments under random budgets: without the compare-and-swap it
  loses about a third of them.
- **The exclusive monitor** is still cleared at every `run()` entry, and
  the grace rule (keep running up to 64 instructions past the budget
  while a monitor is held) still applies per core.

## Shared state on the CPU path

| State | Treatment |
|---|---|
| Predecode block cache (`interp_predecode.c`) | Per host thread. The first thread uses the static cache; each other thread allocates its own once (a pthread key frees it at exit). The flush epoch is one shared atomic counter, so `IC IVAU`, `invalidate_cache` and vmm generation changes reach every thread's cache at its next block. |
| vmm page tables, `g_vmm_generation` | Shared, read lock-free. Changed only in SVCs, under the kernel lock. A core mid-block can run the rest of its block on the old mapping; host memory is one linear memory, so a stale mapping never touches anything outside it. |
| `Interp_State` | Per guest thread; only its own core touches it, apart from wakes (above). |
| Call trace (`interp_set_call_trace`) | Debug-only globals, not thread-safe: use with 0 or 1 core. |
| Logging | `fprintf(stderr)`, natively fine. On the web it is a syscall proxied to the main runtime thread, so log lines from other threads go straight to the console with `emscripten_errn` (`log.c`). |
| JIT backend | Multicore-capable (`supports_multicore`); see "The JIT on several cores" below. |

## The JIT on several cores

Compiled functions live in the function table of the host thread that
installed them (every wasm thread has its own table). So:

- **Per-thread caches.** Each host thread has a `Jit_Thread` (`jit.c`):
  cache, hit counters, module buffer and counters.
  - The first thread uses the static instance; others allocate one,
    released (functions removed) when the thread exits.
  - `jit_stats()` sums the threads; one shared counter struct made a cache
    line bounce between cores on every block entry.
- **Identical bytes, shared code.** Compiled code reads its thread's cache
  base from `Jit_State.thread_cache`, set at every `run()` entry, rather
  than baking an address in. So a region compiles to the same module bytes
  on every core, and V8 shares one compiled module process-wide.
  - Without this, three cores exhausted V8's 4 GiB wasm code space in
    Node.
  - Once any core has compiled a region, other cores compile it on first
    sight in multicore mode (`g_compiled_somewhere`) instead of
    interpreting it up to the hot threshold again.
- **One code generation.** `g_vmm_generation` (vmm.h) moves on every mapping
  change and every code flush, and chained regions check it directly.
  Another core's unmap or remap therefore stops chaining at once; a
  per-thread copy of the generation let a core run stale code.
- **One compilation at a time.** `jit_compile_block` keeps its working state
  in statics, so a mutex in `jit.c` serializes it. Each thread has its own
  output buffer.
- **Code compiled in multicore mode** (each entry records the mode it was
  compiled for; a mode change flushes):
  - STXR/STLXR are an inline `i64.atomic.rmw*.cmpxchg` against the LDXR
    value;
  - DMB/DSB are `atomic.fence`;
  - LDAR/STLR are fenced on both sides, LDAXR after.

## Web

- **Default and override.** `cpu.worker.ts` applies `DEFAULT_HOST_CORES` on
  every load: 3 (the Switch's application cores), capped at
  `navigator.hardwareConcurrency - 2` and at least 1.
  - The same default applies under the JIT.
    - Browser, Silksong gameplay recipe, two interleaved pairs: serial
      14.63 / 14.53 fps vs 3 cores 16.13 / 16.05 fps (+10%).
    - Node, full recipe: 9.87 vs 16.46 frames per wall second (1.67×), now
      that poll coalescing lets the cores overlap in 91% of slices (14%
      before). `?cores=N` on the page URL overrides it through a
  `set-host-cores` message; `?cores=0` is serial.
- **The CPU worker is Emscripten's main runtime thread.** A core's
  syscalls (stderr, anything else not marked `__proxy: none`) are proxied
  to it synchronously, and its proxy queue only runs when asked.
  - While it waits for a slice it therefore waits in 1 ms steps and
    calls `emscripten_current_thread_process_queued_calls()` between
    them.
  - Without this, a core's first log line deadlocks the slice.
- **Threads come from the prepopulated pool** (`PTHREAD_POOL_SIZE=8`, §24).
  `emulator_set_host_cores` shrinks the software renderer's pixel workers
  by as many threads as the cores take, so the total never exceeds the
  pool. A `pthread_create` past the pool would need the CPU worker to
  return to its event loop, which it doesn't while waiting for a slice.
- **Core stacks** are 4 MB (`PARALLEL_CORE_STACK_BYTES`); SVC handlers run
  on them.

## Tests

- `tests/parallel_test.c` (ctest `parallel_test`) checks four things:
  - One core gives exactly the serial results for all guest programs.
  - Two and three cores complete every program.
  - `tests/guest/atomics.s` runs on three host threads, 20 rounds:
    three workers increment shared 32- and 64-bit counters with
    LDAXR/STLXR and LDXR/STXR loops plus DMB, and a lost update fails
    it. With the CAS disabled it fails.
  - Host calls from a core run on the driver.
- `tests/parallel_wasm` builds the same test with the browser's Emscripten
  flags (wasm64, pthreads) and runs it in Node:
  ```
  cmake --build --preset web --target parallel_test_wasm
  node build/web/tests/parallel_wasm/parallel_test.js
  ```
  It also builds `voland-cli` for Node (`parallel-cli-node`) for measuring
  in V8.

## Measuring

On a busy machine, wall time says little: the cores compete with
everything else. `voland-cli --host-cores N` therefore ends with the
scheduler's own account:

```
voland-cli: parallel: S slices, C guest cycles over a span of P (C/P x), F% of slices on 2+ cores
```

- **C** sums every core's guest cycles.
- **P** sums, per slice, the busiest core's cycles: the critical path if
  the host cores were free.
- **C / P** is the parallelism the guest actually offers. It is the
  ceiling on the wall-time speedup (less the per-slice handoff, about 6%
  of host instructions at a 100k-cycle budget with one core).

Snapshot jobs (`--snapshot-at`) take `cores N`, so serial and parallel
runs can start from the same game state:

```
printf 'max_slices 4850000\ncores 2\nlog /tmp/two.log\n' > DIR/job
```

In the browser, `?cores=N` selects the mode, and the homebrew e2e spec
passes `VOLAND_PAGE_QUERY` through to the page.

## Not done / limits

- The JIT backend stays serial until its code cache is per-thread or safe
  to share.
- A paused thread finishes its current run (up to one budget) before
  SetThreadActivity returns. There is no backend hook to stop a run early.
- The pacing change above is deliberate: more guest work per virtual
  second. A title that measures its own CPU time against the tick counter
  sees two cores' worth.

## Pollers and priority (tried, reverted 2026-10-25)

The pick puts a thread marked polling (`Sched_Thread.spinning`: short runs
ending in a yield) after every working thread, whatever the priorities.
That deviates from Horizon: on one core a lower priority never runs while
a better thread can.

- **Priority first, polling only as a tie-break** (5955d7b, reverted): Animal
  Crossing got past a native-only stall. There, main (core 0, priority 44),
  marked polling, lost to a priority-45 core-0 worker that cleared the job
  manager's completion event main then waited on forever. But Tears of
  the Kingdom in the browser never finished loading under wall-clock
  pacing (18 shaders, black, presenting at 55 fps).
- **A two-level per-core pick** (a leader per guest core by priority, then
  working leaders before polling ones): the same TOTK result.
- Without pacing (`?pacing=0`) both reach TOTK's 3D (368 shaders, 3.0 fps).
  Natively, serial TOTK makes 99 presents by 300k slices with them vs ~60.
  `VOLAND_SERIAL_ROTATE=3` no longer crashes with them either. So the
  open question is how pacing's time jumps starve TOTK's lower-priority
  workers when pollers keep their priority. TOTK's pollers yield with
  `SleepThread(-1)` and sleep 20 us / 15 ms between bursts.
  `voland-cli --pacing` paces the CLI, but the CLI does not wait wall time
  between idle slices.
- Paced native A/B (`voland-cli --pacing`, 4 wall minutes). With the old
  rule TOTK reaches 3D (166 shaders) by 178 s virtual. With priority first
  it presents ~20x more often (19723 vs 924 binder transactions) and reads
  less data. Its high-priority threads keep the one host thread busy at
  60 Hz, the lower-priority loaders get what is left, and under pacing
  idle time is no longer fast-forwarded to make up for it.
  - A faithful pick must charge a poller's spinning to its own guest core
    only. The two-level pick tried that and still stalled in the browser.
  - The run trace under pacing shows nothing deadlocked: TOTK's
    priority-56 background loaders run full slices of real parsing work.
    Faithful priorities simply let main run the loading screen at ~55 fps,
    as on its own Switch core. On one host thread that frame loop eats the
    CPU the loaders need, and loading crawls. The old rule throttled it by
    accident, demoting main as a "poller". The faithful fix is several host
    cores, which for TOTK waits on the multicore crash.

**Parallel cores use the per-core rule** (2026-10-25). With two or more host
cores the pick takes each guest core's leader by priority first (then
working before polling, then home core, round-robin). Among the leaders a
host core may run, working ones come before pollers. Serial keeps the
old pick.

- Tears of the Kingdom, three cores, 300k slices: 7 of 9 runs clean
  (~1430-1475 presents), 1 loader crash, 1 stall at boot. The stall was a
  CreateAndDeleteThread busy-polling `TryPeekLightMessageQueue` with no SVC,
  leading its core forever. Before: about 3 of 4 runs crashed.
- Silksong, Celeste, Mario Kart 8 and Animal Crossing on three cores: the
  same presents and CPU time within noise.

**Deterministic repro of the residual TOTK crash** (2026-10-25):
`VOLAND_SERIAL_ROTATE` now orders each core's threads like the parallel pick
(`leads_core`, priority first). `VOLAND_BUDGET_JITTER=3
VOLAND_SERIAL_ROTATE=3 voland-cli ... --host-cores 0` crashes identically at
slice 577966: CreateAndDeleteThread in `RelocationTable::Relocate` with a
null object (sdk+0x7d934 reading 0x4). Seed 0 runs clean to 900k slices.

**`VOLAND_HOLD_CORE`** (2026-10-26, on by default; `=0` turns it off): a thread its budget
preempted stays first among its own guest core's equal priorities, as on
Horizon, where equal priorities never time-slice (a thread keeps its core
until it waits or yields; yields and wakes go to the back). Leaders of
different guest cores still take turns. With it the deterministic repro
(`JITTER=3 ROTATE=3`, and seed 7) runs clean to 700k slices (575-606
presents; without it: crash at 577966, 413 presents). On three host cores
6 of 7 runs were clean to 400k slices (~1800-1900 presents), 1 crashed
early (126k slices, the same null object) - about the old rate, so time
slicing explains the deterministic repro but not all of the parallel crash.
Higher-priority wakes still wait for the end of a slice on parallel cores
(Horizon preempts at once).

**Preemption disable in TLS** (2026-10-26, always on). TOTK's SDK
spinlocks raise the u16 at TLS+0x100 while held, and Horizon never
preempts a thread while it is nonzero: it sets TLS+0x102 and the thread
calls SynchronizePreemptionState (SVC 0x36) when its count is back to 0.
Voland preempted such threads at the end of a slice, so a spinlock holder
could be switched out with a better thread on its core spinning for the
lock (with immediate preemption, below, every early TOTK run stalled
that way). Now a slice that ends with the count nonzero lets the thread
run on, first on its core, and sets the flag; a wake does not stop it
either. tests/guest/preemptoff.s checks it at budgets down to 7 cycles.

**`VOLAND_PREEMPT=1`** (opt-in): on parallel cores a woken thread better
than the one running as its guest core stops that run at once
(`CPU_Backend.request_stop`, within a block) instead of at the end of the
slice. TOTK with it and `VOLAND_HOLD_CORE`: 138-174 presents per 100k
slices vs 100-142 without.

**Hold on by default, immediate preemption opt-in again** (2026-10-26).
Both went on by default first; in the browser (JIT backend, three cores)
Tears of the Kingdom then crashed in 2 of 4 150-s runs, early (null
loads in game code), and in 0 of 3 with only `VOLAND_PREEMPT` off, at the
same 7.5-7.7 fps. The native A/B below, on the interpreter, had shown
nothing. Hold stays on. A/B on three host cores, 200k slices,
defaults vs `VOLAND_HOLD_CORE=0 VOLAND_PREEMPT=0`: Hollow Knight 65601 vs
64682 draws, Animal Crossing and Smash both run on, Mario Kart 8 stops at
the same pre-existing fault (slice ~62k, no input) in both modes. Celeste
and Silksong first looked starved (332 vs 4031 draws), but repeat runs of
the old mode land on the same 332: whether a load finishes inside the
window varies run to run, not by mode.
