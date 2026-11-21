# Voland

A Nintendo Switch emulator that runs in your browser. A C11 core compiled to
WebAssembly, its own ARM64 → WebAssembly JIT, and a WebGPU renderer. Nothing
to install, no keys, and most games need no firmware.

> **Status:** Phase 4 (First Boot). All nine games we test play at full or near-full speed during actual play on an M4 Mac mini: Hollow Knight, Silksong, Celeste and Nine Sols, plus Mario Odyssey, Mario Kart 8 Deluxe, Smash Bros. Ultimate, Animal Crossing and Tears of the Kingdom. Recent work fixed the crash that hit about one Tears of the Kingdom boot in 15 (a kernel synchronisation bug; 40 boots in a row have been clean since), its over-exposed picture, and the shaders behind the black silhouettes and blocky light at Lookout Landing. Still open: Tears of the Kingdom's low frame rate, rare Mario Kart freezes, and Animal Crossing's black sea patches. The full task list is in [DESIGN.md §25](docs/DESIGN.md#25-development-phases), per-game details in [NEW_GAMES.md](docs/NEW_GAMES.md).

### Where each game stands

Measured in Chrome on an M4 Mac mini with default settings, **during play**
(in a level, a race or a fight, not on title screens). "Speed" is how fast
the game world runs against real time: 100% is full speed, below that the
game is in slow motion. fps is the frames shown per second; a game's own
cap is 60 unless noted.

| Game | Where it was measured | Speed | fps | Known problems |
|---|---|---|---|---|
| Hollow Knight | Greenpath, City of Tears | 100% | 49-57 | none known |
| Hollow Knight: Silksong | Bellhart, Bonetown | 100% | 57-58 | none known |
| Celeste | Chapter 3 (Celestial Resort) | 100% | 60 | none known |
| Nine Sols | the first cave | 100% | 45 | |
| Super Mario Odyssey | ten kingdoms, walking (Cap, Cascade, Sand, Wooded, Lake, Seaside, Snow, Luncheon, Bowser's, Moon) and New Donk City | 92-100% in most, 82-99% in Luncheon | 37-60 by kingdom | a kingdom's first seconds stutter while its shaders compile; the camera tutorial needs the right stick pushed up |
| Mario Kart 8 Deluxe (32-bit) | Grand Prix races, all four races of a cup | 98-100% | 45-58 in races | the first visit to a course pauses while its shaders are built (about 25 s before the first race); rare crashes or freezes during Grand Prix runs |
| Super Smash Bros. Ultimate | Battlefield fights | 94-100% | 32-59 (depends on the fight) | none known |
| Animal Crossing: New Horizons | arrival on the island | 100% | 29 (the game's cap is 30) | some of the island's sea renders black in patches; generating the island takes a few minutes |
| Tears of the Kingdom | the cave beneath Hyrule Castle, Great Sky Island, Lookout Landing at night | 99-100% | 12-13 in the cave, 8-9 at Lookout Landing (the game's cap is 30) | a cyan tint on the characters' legs in the cave; faint rectangular patches of different shading at Lookout Landing |

On an M1 Mac, Silksong gameplay runs at about 33 fps.
Only macOS has been tested. Phones: Chrome on Android (64-bit, with WebGPU) should run it - the page fits a phone and plays with a paired controller (there are no touch controls) - but it has only been checked in Chrome's phone emulation, and a phone must grant the fixed 5.25 GB of memory. iPhones and iPads cannot run it yet, in Safari or Chrome: every iOS browser, Chrome included, uses Safari's engine, which lacks the 64-bit WebAssembly memory Voland needs, and iOS allows a tab far less than 5.25 GB. The numbers come from `platform/web/tools/perf.mjs`,
which loads a game in Chrome, plays a scripted input sequence and reports fps,
so you can rerun them with your own game files.

### What works today

- **Commercial games in the browser**, with sound and video cutscenes (see
  the table).
- **Save states.** Freeze the whole machine mid-game and go back to that moment later.
- **Game saves that persist.** Each game's save directory lives in your browser.
  Browse it, drag files in, back it up and restore it.
- **Game library.** Load a game once, then launch it from a tile with one click.
- **Mods, updates and DLC.** Drop in mod folders, .zip or .7z archives: replaced game files
  (LayeredFS), IPS and pchtxt patches, replacement executables, Atmosphere
  cheats you can toggle while playing, and decrypted game-update and DLC NCAs. Voland ships no mods; see [docs/MODS.md](docs/MODS.md).
- **Remappable controls.** Keyboard or any standard gamepad, rebound from the
  controls panel.
- **Docked or handheld, per game.** Docked games draw at a higher resolution
  (Tears of the Kingdom at 1600x900 instead of 1280x720). The setting
  applies when the game starts.
- **Fast paths.** An ARM64 → WebAssembly JIT, guest threads spread across host
  cores, a WebGPU renderer, and a shader cache that warms up on later visits.
- **Homebrew.** NROs run directly, or through a homebrew menu from the emulated
  SD card.
- **A compatibility report.** One click collects every warning and error the
  core logged, which usually shows what a new game is missing.

### Is it really running the game?

Yes, and you can check that without taking our word for it:

- **It has nothing to play without your files.** This repository and the
  hosted app contain no game code, art or sound. The whole app is about
  4.5 MB, 2 MB of it WebAssembly, while one Switch game is several
  gigabytes. Until you load a game you dumped yourself, only homebrew and
  the built-in demo run.
- **Your own save picks up where you left off.** Drag a save from your
  Switch into the app's save browser and the game continues from your progress,
  your location and your gear. A recording could not do that.
- **The code is a whole emulator.** About 74,000 lines of C. 18,000 of
  them emulate the CPU: interpreters for 64-bit and 32-bit ARM, plus the
  JIT that turns the game's ARM code into WebAssembly. 27,000 rebuild the
  Switch's operating system: the kernel and services such as file system,
  input, audio and display.
  21,000 are the GPU: they process the game's Maxwell command stream and
  translate its compiled shaders to WebGPU's shading language. 23,000 more
  lines are tests (71 suites). They include real ARM64 programs run on the
  emulated kernel, and differential tests that check every JIT block
  against the interpreter. The [design document](docs/DESIGN.md) and
  [JIT.md](docs/JIT.md) explain how the pieces fit.
- **There are no per-game code paths.** The core never checks which game
  is running. The one per-game setting is a thread-placement default for
  Hollow Knight in `platform/web/workers/cpu.worker.ts`. To check, search
  for game IDs:
  `grep -rn "0100[0-9A-Fa-f]\{12\}" core platform/web/src platform/web/workers`.
  The other hits are a system archive, a range limit and a comment.
- **Its bugs are emulator bugs.** The *Known problems* column above lists
  them: a cyan tint on characters' legs, black patches in one game's sea,
  rare crashes when guest threads run on several cores. Each one is
  chased down to a part of the emulator, such as shader translation or
  kernel synchronisation. The commit history shows how the fixed ones
  were found and fixed, and
  [PARALLEL.md](docs/PARALLEL.md) and [NEW_GAMES.md](docs/NEW_GAMES.md)
  record the investigations.
- **You can rerun the numbers.** `platform/web/tools/perf.mjs` loads your
  copy of a game in Chrome, plays scripted input and reports speed and fps.
  The in-app compatibility report lists every warning and error the core
  logged while the game ran, such as the system calls and services it used
  that are missing or incomplete.

---

## Quick start

### Use it in the browser

Open **https://helloimthesigma-blip.github.io/Voland/** in Chrome or Edge, then load
a decrypted game file (see [What you need](#what-you-need)). The first visit
reloads itself once to turn on the browser features the emulator needs.

### Run it on your own machine

```bash
git clone https://github.com/helloimthesigma-blip/Voland
cd Voland
./voland
```

`./voland` checks your tools, builds what is missing, and opens the app at
`http://localhost:5174`. The first build takes a few minutes; later runs
start in seconds. It needs:

- **CMake** 3.24+, **Ninja**, a C compiler (Clang 16+ or GCC 12+)
- **Node.js** 22.6+
- **Emscripten** 6.0.9+. If it is missing, `./voland` offers to install it
  into `.emsdk/` inside the checkout.

On macOS: `brew install cmake ninja node`. On Debian/Ubuntu:
`sudo apt install cmake ninja-build clang nodejs`.

Other commands: `./voland dev` (live-reloading dev server), `./voland build`,
`./voland test`, `./voland cli ...` (native command-line runner), and
`./voland help`.

### Browser and hardware

- **Chrome or Edge** (desktop, recent). The emulator needs WebGPU, 64-bit
  WebAssembly memory and cross-origin isolation. Other browsers are untested.
- About **6 GB of free memory**. The emulator reserves the Switch's full
  memory up front.
- A fast CPU. Speed comes mostly from single-thread performance plus up to
  3 extra cores for game threads.

---

## What you need

Voland loads **decrypted NCA files only**: the game's *Program* NCA, the
largest one in a dump. It never asks for keys, never reads NSP or XCI files,
and contains no decryption code. You dump and decrypt the game yourself,
from a Switch you own, with separate tools. The [dumping guide](docs/DUMP.md)
walks through it.

**Keep it all in one folder.** Choose a *Voland folder* in the app (under
*Your files*) with `games/`, `firmware/` and `mods/` in it. Voland finds your
games, their updates and DLC, the system files a few games need, and your
mods, and shows a checklist of anything missing or still encrypted. The
[dumping guide](docs/DUMP.md#step-6---provide-to-voland) shows the layout.

Homebrew `.nro` files load directly. The app also has a built-in demo.

**Controls:** gamepads work out of the box (standard layout). Keyboard:

| Switch | Key | Switch | Key |
|---|---|---|---|
| A / B / X / Y | Z / X / C / V | Left stick | W A S D |
| L / R | E / U | Right stick | I J K L |
| ZL / ZR | Q / O | D-pad | Arrow keys |
| + / − | = / - | Stick clicks | F / N |

**Pick up where you left off.** Voland saves the whole game by itself
every 10 minutes (Settings, *Resume point*), so a game's library tile
offers *Continue*, and a crash offers *Go back*. Frame skip and threads
you change while a game runs are kept for that game.

**Saves** are kept in the browser automatically. Use *Back up saves* in the
app to download them and *Restore saves…* to bring them back, for example
on another machine.

**Options** (add to the URL): `?pacing=0` runs as fast as possible instead of
at game speed, which is useful for benchmarking.

---

## How it works

```
core/                 C11, compiled to wasm64 (web) or native
  cpu/                backend interface + interpreter + ARM64→wasm JIT
  hle/                Horizon OS: kernel (threads, sync, memory, IPC) and services
  gpu/                Maxwell command processing, shader → WGSL translation
  video/, audio/      NVDEC/VIC video path, audio output
  common/             fixed memory layout, softmmu, arena, logging
platform/
  web/                Solid.js app, workers, service worker, WebGPU executor
  cli/                native command-line runner (tests, benchmarks)
tests/                C unit and integration tests (ctest)
docs/                 design and reference documents
```

Three decisions shape everything ([DESIGN.md §1](docs/DESIGN.md)):

1. **One linear memory.** Guest RAM is a region inside a single fixed-size
   WebAssembly memory shared by all workers.
2. **A softmmu.** Every guest memory access goes through page-table
   translation, because WebAssembly cannot alias pages.
3. **Budgeted CPU backends.** A backend runs a guest thread for a cycle
   budget and returns. Nothing blocks; guest threads are green threads,
   spread over up to 3 host cores.

| CPU backend | Status | Notes |
|---|---|---|
| noop | Complete | Default for native builds; tests everything except guest code |
| interpreter | Working | ARMv8.0 user ISA; the reference every JIT block is tested against |
| jit | Working | ARM64 → WebAssembly, the web default ([docs/JIT.md](docs/JIT.md)) |

More detail: [DESIGN.md](docs/DESIGN.md) (architecture),
[JIT.md](docs/JIT.md), [PARALLEL.md](docs/PARALLEL.md) (multi-core guest
threads), [GPU_COMMAND_STREAM.md](docs/GPU_COMMAND_STREAM.md).

---

## Building by hand

`./voland` wraps these steps.

```bash
# Native (no CPU emulation; runs the test suite)
cmake --preset native-noop && cmake --build --preset native-noop
ctest --preset native-noop

# Native command-line runner (interpreter)
cmake --preset native-release && cmake --build --preset native-release
build/native-release/platform/cli/voland-cli run game.nca --backend interpreter

# Web core (needs Emscripten activated: source <emsdk>/emsdk_env.sh)
cmake --preset web && cmake --build --preset web

# Web app
cd platform/web
npx pnpm@12.8.1 install --frozen-lockfile
npm run typecheck && npm test   # unit tests
npm run e2e                     # browser tests (headless Chromium)
npm run build && npm run preview
```

The production build is static files plus a service worker that supplies
the required cross-origin isolation headers, so it can be hosted anywhere,
including in a subdirectory: `VOLAND_BASE=/path/ npm run build`.

---

## Contributing

Read [CONTRIBUTING.md](CONTRIBUTING.md) first. The short version:

- **No decryption code, keys, firmware or game files**, in the tree or in
  history, ever.
- **No code copied from other emulators.** Study them for behavior,
  reimplement from that.
- Core is C11 with explicit `Error` returns. TypeScript is strict with no `any`.
- Tests land with the code. The interpreter is the reference for the JIT.

---

## Legal

Voland is open source software released under the [GPL-2.0 license](LICENSE).

Voland does not include, distribute, or help obtain Nintendo's copyrighted
material. Users dump and decrypt game files themselves, with separate tools,
from hardware they own. Voland accepts only the resulting decrypted NCA
files, never consumes keys, and needs firmware only for the few system
archives some games read, which users also dump and decrypt themselves (see
[DESIGN.md §1.6](docs/DESIGN.md#16-legal-scope-boundaries)). Voland never
connects to Nintendo's servers.

Voland is not affiliated with Nintendo Co., Ltd.

---

## About the name

Voland is named for Völundr (Old Norse, also Wayland the Smith in
Anglo-Saxon tradition), the legendary craftsman of Germanic and Norse
mythology and master of the forge. It is also Mikhail Bulgakov's name for the
devil in *The Master and Margarita*. The Norse origin is the reason for the
name; the literary echo is welcome.

## Credits

Voland was created by **Proxy** and **Null6598** at
[voland-emu/Voland](https://github.com/voland-emu/Voland), and everything
here is built on the foundation they laid:

- **Proxy** ([proxy-alt.dev](https://proxy-alt.dev)) started the project:
  the original proof of concept, the design document
  ([DESIGN.md](docs/DESIGN.md)) with its web-first architecture (one linear
  memory, the softmmu, bounded CPU backends, the WebGPU renderer and the
  worker split), Phase 0's build and web platform, the linear-memory
  layout and the CPU backend contract.
- **Null6598** built the first working core pieces: the softmmu, decrypted
  NCA parsing, the NSO loader and bootstrap, the TLS allocator and the
  memory HLE.

Thank you both.

## Acknowledgements

- [voland-emu/Voland](https://github.com/voland-emu/Voland), the project
  this repository grew from
- [Ryubing](https://github.com/Ryubing), yuzu and
  [dynarmic](https://github.com/merryhime/dynarmic): behavior references
- [libnx](https://github.com/switchbrew/libnx) and
  [switchbrew](https://switchbrew.org): documentation of the Switch's
  interfaces
- [Noto Sans](https://fonts.google.com/noto) (SIL OFL 1.1) as the system font
