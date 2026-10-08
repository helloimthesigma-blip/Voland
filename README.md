# Voland

A Nintendo Switch emulator that runs in your browser. A C11 core compiled to
WebAssembly, its own ARM64 → WebAssembly JIT, and a WebGPU renderer. Nothing
to install, and no keys or firmware.

**Hollow Knight and Hollow Knight: Silksong are fully playable in a browser
tab**: real commercial Switch games booting, running cutscenes and playing at
full game speed, at a stable ~15 fps on a fast desktop.

> **Status:** Phase 4 (First Boot). Silksong and Hollow Knight run at real speed (~22 fps in gameplay), and Silksong saves no longer crash. Super Smash Bros. Ultimate plays matches with correct colours and saves, now at 11-14 fps in a fight (was ~4): vertex pulling, parallel compute and cheaper GPU uploads. Its game speed still follows its frame rate (frame skip raises it; serial guest threads are faster for it). Not done yet: VP9 video and Opus audio (SSBU movies are black and silent); full task list in [DESIGN.md §25](docs/DESIGN.md).

### What works today

- **Commercial games in the browser.** Hollow Knight and Hollow Knight: Silksong
  play from boot to gameplay at real game speed, with sound and video cutscenes.
- **Save states.** Freeze the whole machine mid-fight and jump back in a second.
  They survive a page reload.
- **Game saves that persist.** Each game's save directory lives in your browser.
  Browse it, drag files in, back it up and restore it.
- **Game library.** Load a game once, then launch it from a tile with one click.
- **Remappable controls.** Keyboard or any standard gamepad, rebound from the
  controls panel.
- **Fast paths.** An ARM64 → WebAssembly JIT, guest threads spread across host
  cores, a WebGPU renderer, and a shader cache that warms up on later visits.
- **Homebrew.** NROs run directly, or through a homebrew menu from the emulated
  SD card.
- **A compatibility report.** One click tells you exactly what a new game is
  missing.

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

Homebrew `.nro` files load directly. The app also has a built-in demo.

**Controls:** gamepads work out of the box (standard layout). Keyboard:

| Switch | Key | Switch | Key |
|---|---|---|---|
| A / B / X / Y | Z / X / C / V | Left stick | W A S D |
| L / R | E / U | Right stick | I J K L |
| ZL / ZR | Q / O | D-pad | Arrow keys |
| + / − | = / - | Stick clicks | F / N |

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
files and never consumes keys or firmware (see
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

## Acknowledgements

- [voland-emu/Voland](https://github.com/voland-emu/Voland), the project
  this repository grew from (design, web platform and early core)
- [Ryubing](https://github.com/Ryubing), yuzu and
  [dynarmic](https://github.com/merryhime/dynarmic): behavior references
- [libnx](https://github.com/switchbrew/libnx) and
  [switchbrew](https://switchbrew.org): documentation of the Switch's
  interfaces
- [Noto Sans](https://fonts.google.com/noto) (SIL OFL 1.1) as the system font
