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
