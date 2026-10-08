# Asynchronous GPU

GPU command processing on its own host thread, beside the guest
(`core/gpu/gpu_thread.{h,c}`). The browser turns it on by default
(`?gpuasync=0` turns it off). Natively it is off, which keeps runs
deterministic for tests; `voland-cli --gpu-async` or
`emulator_set_gpu_async` turns it on.

## Why

On the Switch, a GPFIFO submission returns as soon as its entries are
queued. The GPU runs them later and signals syncpoints as it finishes.
Voland ran every submission synchronously, inside the submitting guest
thread's ioctl. That covered the 3D engine, compute, texture decoding and
streaming draws to the GPU worker.

In a Super Smash Bros. Ultimate fight (serial scheduler, pacing off),
that was 38.6% of the CPU worker's time. Every guest thread waited for
it: the guest's threads hand work to each other, so on three host cores
each core was busy only about 40% of the time.

## Model

- **The queue.** Work for the renderer goes through `gpu_thread_call(fn,
  payload)`. Asynchronously, the payload is copied into an 8 MiB FIFO and
  the call returns. The GPU thread runs the calls one at a time, in
  order. Synchronously (natively, by default), the call runs at once on the
  caller, exactly as before, so runs stay deterministic and frame hashes
  and save-state checks hold.
- **What is queued:**
  - GPFIFO submissions (`nvdrv.c` `run_gpfifo` → `gpfifo_run`);
  - display presents (`vi.c` `composite` → `composite_run`), which come
    after the submissions that drew the frame, as the display engine
    reads a finished buffer;
  - frame ends (`frame_end_run`), which carry the frame-skip decision
    for the next frame. vi makes the decision at queue time, so it
    applies to the right frame's draws.
- **What waits instead.** The ioctls that change what command processing
  reads hold `gpu_thread_lock`, so the GPU thread is between calls while
  they run. These are nvmap handles, GPU mappings, and the multimedia
  engines' writes into the renderer's textures. The GPU thread holds the
  lock for each call.

## Syncpoints

A submission promises increments when it is queued; it does not wait to
be processed.

- **Promised increments.** Flag `0x100` (increment value) promises
  `fence.value` increments. `FENCE_GET` (`0x2`) promises the one the
  driver appends. `max` advances by that count at once, in submission
  order. The fence `{id, max}` is returned.
- **Reaching the fence.** On the GPU thread, a promised in-stream
  increment advances only `min`, and never past the submission's own
  fence. That keeps a later submission's fence from being reached early.
  When the submission ends, `min` is raised to the fence whatever the
  commands did, so a missed increment cannot hang the guest.
  Unpromised increments promise and reach at once, as before.
- **Atomics.** `min` and `max` are atomics (`syncpoint.c`). The guest's
  threads read and promise while the GPU thread completes.
- **What SSBU does.** Each submit carries flags `0x104` and one promised
  increment, and its command stream makes exactly one in-stream
  increment of the channel syncpoint (28,000 of 28,000 in a fight).
- **Waking the guest.** Waiters use `EVENT_WAIT_ASYNC` and a kernel event
  (about 5,000 per run in SSBU; the blocking waits are unused). The
  events are signalled from `nvdrv_poll_completions` at slice cadence,
  the path the GPU worker's completion ring already used.

## Idle

When no guest thread can run while the GPU thread still has work, the
guest is waiting for it: a fence, or a present before the next vsync.
`wait_for_gpu` (`emulator.c`) waits in host time for the GPU thread to
make progress, polling the devices after each wait. It does not let
virtual time jump past the work. Synchronously, the GPU's work took no
virtual time either.

## Host-side flow control

Waiting for room in the queue, or for it to drain, is the same kind of
wait as waiting for room in the GPU stream ring (`gpu_stream.h`). No
guest thread is marked waiting, and the queue holds several frames of
work. The queue drains at every point that needs a quiet renderer:

- mode changes (GPU mode, host cores, frame skip off);
- save states;
- program unload;
- the CLI's final counters.

## Threads on the web

The thread comes from the fixed pthread pool (DESIGN.md §24): one pixel
worker gives up its place, as each guest core does.

## Limits

- **Free-running parallel mode.** `wait_for_gpu` runs between slices
  (serial, or parallel slice mode, where every core is parked). Free-running
  cores take their own idle steps and do not wait for the GPU thread.
- **Timing.** The guest sees GPU work take host time. Fences can be
  unreached when the guest first checks them, which never happened
  synchronously. Run-to-run results differ, so the asynchronous mode is
  not used for frame-hash tests.
