# Next: Mario Kart 8 Deluxe at full speed, and online play

Notes for the next round of work (written 2026-10-09, not started). Where
things stand: MK8DX plays whole races in the browser with correct
rendering at ~17 fps serial / ~20 fps on three cores - about a third of
real game speed (the game runs in slow motion, since its logic is tied to
virtual time). Recipes and tools: `docs/JIT.md`, `docs/PARALLEL.md`,
`docs/ASYNC_GPU.md`.

## 1. MK8DX at the right speed

Target: 60 fps worth of game logic per wall second (100% speed), or 30 fps
with frame skip and the game still at 100% speed. Today: ~30-35%. Needs
roughly 3x.

### What the measurements say (race, browser, 2026-10-09)

- **CPU worker: ~89% in JIT-compiled code**, spread thinly (no region over
  2%). The core's C is ~10% (softfloat leftovers, the dispatcher,
  `update_devices` once per slice).
- **Compiled regions are tiny.** 440 M region entries per 950k slices after
  today's call spanning (702 M before), ~6 guest instructions per entry.
  Each entry pays a prologue (registers loaded from the state), an epilogue
  and a chain lookup.
- **Three cores give only ~1.2x** over serial. Each core runs ~50% and is
  parked 30-40%: MK8DX's threads (main, prepare, two workers, presentation)
  wait on each other. The critical path is one thread per frame.
- GPU side is not the limit: the GPU worker is ~85% idle; stream waits
  ~10% in multicore runs.

### Ideas, biggest expected win first

1. **Find what the parked cores wait on.** Add wait-reason accounting to
   the parallel scheduler (which handle/event/futex each thread blocks on,
   and for how long, per frame). If MK8DX's main thread waits for a GPU
   fence or for vsync that virtual time reaches late, the fix is in time
   accounting, not CPU speed. Cheapest big lever if it is there.
2. **Bigger regions / cheaper transitions.** Keep guest registers in wasm
   locals across chained regions (pass them as parameters, or a
   per-region "live-in" set) so short regions stop reloading and spilling
   everything. Profile-guided region formation: after N entries, recompile
   a hot chain (A -> B -> C) as one region.
3. **Return-address prediction for calls whose target is unknown**
   (`blx rN` where the entry-state prediction misses): call the callee
   region as a wasm call instead of a tail call and return straight into
   the caller when the guest returns to the expected address.
4. **Thumb code is all interpreted.** ~14% of MK8DX's code is Thumb; it was
   not hot in profiles, but check per scene (menus, results screens).
5. **Remaining softfloat**: VRSQRTE/VRECPE estimates (table lookups could
   be inlined), any VFP op still on the exact path in hot loops.
6. **Frame skip + correct game speed.** If 100% CPU speed is out of reach,
   make frame skip keep game logic at full speed (render every other frame)
   - check how MK8DX's logic follows vsync first.

Measure with: `~/WORKSPACE/bot2-work/mk8web/race.sh` (serial is
deterministic; `CORES=3` for multicore, but multicore runs differ run to
run - A/B multicore in the CLI with env toggles instead), `--profile 10`
for the CPU profile, `voland-cli ... --jit-hot 25` for region entries.

### Known MK8DX visual issues (separate from speed)

- Black/yellow shards around the player's kart during some item/boost
  effects (reproducible in serial runs mid race 2).
- Occasional black blocky tiles over the track (seen in multicore runs).
- Environment reflections: MK8DX compresses its env cubemap to BC3 on the
  GPU (RGBA32UI targets sampled as BC3); not supported yet.

## 2. Online play

### What is allowed (DESIGN.md §1.6, §20; CLAUDE.md hard rule 9)

- **Never** connect to Nintendo servers, ship Nintendo endpoints/DNS
  names/certificates, or emulate Nintendo's online backend (NPLN, Pia
  session brokering, NSO auth). Nintendo-online-only modes stay dead.
- No aggregation servers, no telemetry, nothing run by us that everyone
  depends on.
- **What works instead: the game's local-wireless mode over the
  internet.** Games like MK8DX have a "local wireless" multiplayer mode
  (consoles in the same room). Voland makes the `ldn:` service carry those
  packets over a WebRTC DataChannel to the other players' browsers. The game
  thinks it is on local Wi-Fi. This is how emulator online play (RyuLDN)
  has always actually worked. MK8DX supports it (up to 8 consoles).

### Where it stands

- `core/hle/services/network/network.c`: stubs for `bsd:` and `nifm:` only.
- No `ldn:` service yet; nothing on the web side (no WebRTC).
- DESIGN.md §20 already describes the design (LDN transport, room codes,
  signalling, optional TURN, the first-use disclosure).

### Plan

1. **`ldn:` HLE service** (`core/hle/services/network/ldn.c`): the
   access-point/station state machine the game drives (create network,
   scan, connect, node list, disconnect), with packets in and out through
   one linear-memory ring (§6: no per-frame postMessage). Test with two
   emulator instances in one process first (the ring wired back to back).
2. **`nifm:` honesty**: report "connected" only while an LDN session is
   active.
3. **WebRTC transport** in a worker: unreliable, unordered DataChannel;
   one peer connection per other player; packets copied between the ring
   and the channel.
4. **Rooms**: a room code the host shares; signalling through a small
   self-hostable server (the user picks it - none shipped or default) or
   copy-paste offer/answer for two players with no server at all.
5. **Lobby UI**: host/join a room, see players, leave. MK8DX: both players
   pick "Wireless Play" in the menu.
6. Then: latency notes (WebRTC is slower than Wi-Fi; MK8DX tolerates it),
   TURN for symmetric NAT (user-configured).

Start with two local browser tabs on one machine (WebRTC loopback), MK8DX
wireless play, before anything crosses the internet.

## 3. Vertex data that stays on the GPU (proposal, 2026-11-08, not started)

**Why.** On the owner's M1, Super Mario Odyssey's frame copies about 7 MB
of draw data that did not change since the last frame: pulled vertex
streams (~3.4 MB), indices (~2.1 MB) and per-draw data (~1 MB), through
the core's GPU thread (memcpy into the ring), the GPU worker
(`writeBuffer`) and Chrome's GPU process. With BCn/ASTC now filtered by
the sampler (476f425) the M1 runs SMO CPU-contended at ~28 fps on its
four performance cores; with pixel shading off it reaches ~41, and the
copies are the part of that gap not spent shading. The mini (12 cores)
does not show it.

**Shape.** Vertex and index ranges live in GPU buffers that mirror guest
memory page by page and are re-uploaded only where the guest wrote:

- *Knowing what was written, without hashing.* `gpu_mirror` hashes each
  page once per submission, but a submission is every GPFIFO submit and
  copy launch (several per frame), so hashing every vertex page that
  often costs about what it saves. Instead, pages under a vertex mirror
  become *watched*: the softmmu walk (`vmm_translate_inline`, and the
  JIT's `FUNC_WALK`) refuses to fill a write translation for them, so a
  guest store there takes the slow path (`jit_helper_store` /
  `interp_write`). That path sets the page's dirty bit (an atomic bitmap
  the GPU thread reads), unwatches nothing, and performs the store. Reads
  are unaffected; a page nobody writes costs nothing.
- *Pages the guest writes every frame* (dynamic vertex buffers, UI) would
  take the slow path on every store. A page dirtied in N consecutive
  frames (N ~ 2) is unwatched and its streams go back to riding inline,
  as today; that decision is per page and can be revisited at a
  generation change.
- *Other writers* already report: DMA and 2D copies and VIC
  (`raster3d_sync_range` -> `gpu_mirror_cpu_wrote`), CPU-run compute
  (`g_compute_writes`); HLE writes through `vmm_write*` need the same
  dirty mark.
- *Unmap / permission change* bumps `vmm_map_generation`; mirrors drop
  pages whose translation changed, as the JIT's carried TLB does.
- *GPU side.* Today one resident buffer per draw (`VS_RESIDENT_BINDING`)
  and 32 mirrors of 4 MiB; SMO's streams are spread over far more memory
  than that. Mirrors would become fixed-size chunks (e.g. 1 MiB, keyed by
  address, LRU over a budget), a draw binds the chunk(s) holding its
  streams, and indices need INDEX usage on the chunk buffers (or a
  separate index chunk set).

**Costs and risks.** The watch check sits on the store walk, the JIT's
hottest path after loads - it must stay one extra bit test in the walk
(not in the cached fast path). Stream format change (a draw referring to
chunk ids and offsets for vertices and indices), a version bump, and
GPU_COMMAND_STREAM.md. Header changes in `core/common/vmm.h` (watch,
dirty bitmap) per the interface rule.

**Measure first.** How many of SMO's vertex/index pages are written per
frame (a counting pass with the watch bit but no GPU change), and the
store-walk cost of the extra bit in `tests/jit_bench.c`.
