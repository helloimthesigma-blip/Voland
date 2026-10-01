/**
 * CPU emulation worker (docs/DESIGN.md section 16). Owns the core WASM
 * module instance and, once the Phase 2 scheduler exists, the guest
 * thread scheduler loop (§7).
 *
 * Instantiation contract: `emcmake cmake --build` (CPU_BACKEND=noop by
 * default, §24) emits `switch_core.js` + `switch_core.wasm` as an
 * EXPORT_ES6 module, and a POST_BUILD step in core/CMakeLists.txt stages
 * both into `platform/web/public/core/` so Vite serves them as static
 * assets. Without a web build the dynamic import below fails and this
 * worker reports a clear "error" message instead of a fake "ready" - the
 * boot path is meant to be exercised honestly, not stubbed into looking
 * alive.
 */

import type { CpuBackendId, GameFileReadHook, SwitchCoreExports } from "@bindings/core";
import { CPU_BACKEND_DISPLAY_NAMES } from "@bindings/core";
import { readMemoryLayout } from "@bindings/layout";
import { CoreResult, formatTitleId, loadFailureFromResult, readCString } from "@bindings/load";
import type { CPUToMainMessage, MainToCPUMessage } from "@bindings/protocol";

const self: DedicatedWorkerGlobalScope =
  globalThis as unknown as DedicatedWorkerGlobalScope;

function log(level: "debug" | "info" | "warn" | "error", message: string): void {
  const msg: CPUToMainMessage = { type: "log", level, message };
  self.postMessage(msg);
}

/** Emscripten EXPORT_ES6+MODULARIZE factory shape: a default export that
 * takes an options object (the imported WebAssembly.Memory for
 * -sIMPORTED_MEMORY, plus the game-file read hook wasm_entry.c's EM_JS
 * looks up on `Module`) and resolves to the instantiated module's exports. */
interface CoreModuleOptions {
  readonly wasmMemory:         WebAssembly.Memory;
  readonly volandReadGameFile: GameFileReadHook;
}
type CoreModuleFactory = (options: CoreModuleOptions) => Promise<SwitchCoreExports>;

const CORE_MODULE_URL = "/core/switch_core.js";

/** Upper bound on a core Error message (static strings, all short). */
const CORE_ERROR_MESSAGE_MAX_BYTES = 1024;

let core: SwitchCoreExports | null = null;
let coreMemory: WebAssembly.Memory | null = null;

/* The file the core is currently reading; set only for the duration of
 * a load-game request. The loader is synchronous (byte_source.h), so a
 * single slot suffices. */
let activeGameFile: File | null = null;
const fileReader = new FileReaderSync();

/** byte_source.h `read` realized over FileReaderSync + `blob.slice()`
 * (§15): synchronous, exact-length, straight into linear memory. */
const readGameFile: GameFileReadHook = (offset, destination, size) => {
  if (!activeGameFile || !coreMemory) return false;
  try {
    const bytes = fileReader.readAsArrayBuffer(activeGameFile.slice(offset, offset + size));
    if (bytes.byteLength !== size) return false;
    new Uint8Array(coreMemory.buffer, destination, size).set(new Uint8Array(bytes));
    return true;
  } catch (e) {
    log("warn", `game file read failed at offset ${offset}: ${e instanceof Error ? e.message : String(e)}`);
    return false;
  }
};

async function loadCoreModule(memory: WebAssembly.Memory): Promise<SwitchCoreExports> {
  /* An absolute URL, not the bare "/core/..." path: Vite's dev server
   * appends `?import` to root-relative dynamic imports and then refuses
   * to serve files under public/ as modules ("can only be referenced via
   * HTML tags"). A full URL bypasses that rewrite in dev and is
   * equivalent in a production build. */
  const coreUrl = new URL(CORE_MODULE_URL, self.location.origin).href;
  const factory = (await import(/* @vite-ignore */ coreUrl)).default as CoreModuleFactory;
  return factory({ wasmMemory: memory, volandReadGameFile: readGameFile });
}

function randomAslrSeed(): bigint {
  const words = crypto.getRandomValues(new BigUint64Array(1));
  const seed = words[0] ?? 1n;
  return seed === 0n ? 1n : seed; // 0 disables ASLR (address_space.h)
}

function loadGame(file: File): CPUToMainMessage {
  if (!core || !coreMemory) {
    return { type: "load-failed", failure: { reason: "internal", message: "the emulator core is not loaded" } };
  }
  log("info", `loading ${file.name} (${file.size} bytes)`);

  /* One program per Emulator (emulator.h): replace whatever was loaded. */
  core._emulator_unload_program_ffi();

  activeGameFile = file;
  let code: number;
  try {
    code = core._emulator_load_program_ffi(BigInt(file.size), randomAslrSeed());
  } finally {
    activeGameFile = null;
  }

  if (code !== CoreResult.Ok) {
    const message = readCString(coreMemory.buffer, Number(core._emulator_last_error_message_ffi()),
                                CORE_ERROR_MESSAGE_MAX_BYTES);
    return { type: "load-failed", failure: loadFailureFromResult(code, message) };
  }

  return {
    type: "game-loaded",
    titleId: formatTitleId(core._emulator_program_id_ffi()),
    entryPoint: core._cpu_get_pc_ffi(),
  };
}

async function init(memory: WebAssembly.Memory): Promise<void> {
  log("info", "cpu.worker initialising");

  coreMemory = memory;
  try {
    core = await loadCoreModule(memory);
  } catch (e) {
    const err: CPUToMainMessage = {
      type: "error",
      message: `failed to load core module from ${CORE_MODULE_URL}: ${e instanceof Error ? e.message : String(e)}. ` +
        `Build it with cmake --preset web && cmake --build --preset web (docs/DESIGN.md §24); ` +
        `the build stages switch_core.{js,wasm} into platform/web/public/core/.`,
    };
    self.postMessage(err);
    return;
  }

  if (core._emulator_create_ffi() === 0) {
    const err: CPUToMainMessage = { type: "error", message: "emulator_create_ffi failed" };
    self.postMessage(err);
    return;
  }

  const layoutPtr = core._layout_get_ffi();
  const layout = readMemoryLayout(memory, layoutPtr);
  const layoutMsg: CPUToMainMessage = { type: "layout", layout };
  self.postMessage(layoutMsg);

  const backendId = core._cpu_backend_id_ffi() as CpuBackendId;
  const backendName = CPU_BACKEND_DISPLAY_NAMES[backendId] ?? `unknown(${backendId})`;
  log("info", `guestRamSize=${layout.guestRamSize} bytes, backend=${backendName}`);

  const ready: CPUToMainMessage = {
    type: "ready",
    backendName,
    backendVersion: "1.0.0",
  };
  self.postMessage(ready);
}

self.addEventListener("message", (event: MessageEvent<MainToCPUMessage>) => {
  const msg = event.data;

  if (msg.type === "init") {
    init(msg.memory).catch((e: unknown) => {
      const err: CPUToMainMessage = {
        type: "error",
        message: `init threw: ${e instanceof Error ? e.message : String(e)}`,
      };
      self.postMessage(err);
    });
    return;
  }

  if (msg.type === "load-game") {
    self.postMessage(loadGame(msg.file));
    return;
  }

  if (msg.type === "pause" || msg.type === "resume") {
    // No scheduler yet (§7 is Phase 2); acknowledged so main.ts's
    // visibilitychange handler has somewhere real to send these.
    log("debug", `${msg.type} (no-op until the Phase 2 scheduler exists)`);
    return;
  }

  if (msg.type === "halt") {
    const halted: CPUToMainMessage = { type: "halted" };
    self.postMessage(halted);
    return;
  }
});
