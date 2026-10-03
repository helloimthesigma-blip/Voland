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

import type { CpuBackendId, GameFileReadHook, GuestOutputHook, SwitchCoreExports } from "@bindings/core";
import { CPU_BACKEND_DISPLAY_NAMES } from "@bindings/core";
import { readMemoryLayout } from "@bindings/layout";
import { CoreResult, formatTitleId, loadFailureFromResult, readCString, runStateAfterSlice } from "@bindings/load";
import type { CPUToMainMessage, MainToCPUMessage } from "@bindings/protocol";
import { clearSdFiles, diffManifests, parseManifest, persistSdFile, removeSdFile, restoreSdFiles } from "./sd-persistence";
import { parseTextInputRequest } from "./text-input";

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
  readonly volandGuestOutput:  GuestOutputHook;
}
type CoreModuleFactory = (options: CoreModuleOptions) => Promise<SwitchCoreExports>;

const CORE_MODULE_URL = "/core/switch_core.js";

/* pl:u's system font (§1.6: an openly licensed font - Noto Sans, SIL OFL
 * 1.1, see public/fonts/OFL.txt - never Nintendo's). */
const SHARED_FONT_URL = "/fonts/NotoSans-Regular.ttf";

/** Upper bound on a core Error message (static strings, all short). */
const CORE_ERROR_MESSAGE_MAX_BYTES = 1024;

let core: SwitchCoreExports | null = null;
let coreMemory: WebAssembly.Memory | null = null;

/* The loaded program's file. The core reads it while the program runs
 * (fsp-srv serves the RomFS from it, emulator.h), so it stays attached
 * until the next load replaces it. */
let activeGameFile: File | null = null;
const fileReader = new FileReaderSync();

/** byte_source.h `read` realized over FileReaderSync + `blob.slice()`
 * (§15): synchronous, exact-length, straight into linear memory. */
/* Game-file read-ahead. Each FileReaderSync read costs a fixed trip to
 * the browser's blob store (most of a small read's time), and the core
 * reads RomFS in small, mostly sequential pieces, so reads go through
 * READ_CHUNK_BYTES-aligned chunks kept in a small LRU (plain worker-heap
 * ArrayBuffers the core never sees; bytes are copied into linear memory). */
const READ_CHUNK_BYTES = 1 << 20;
const READ_CACHE_CHUNKS = 32;
/** Reads bigger than this skip the cache (one direct read). */
const READ_DIRECT_BYTES = 4 * READ_CHUNK_BYTES;
/** Chunk index -> bytes, in least- to most-recently used order. */
const readCache = new Map<number, Uint8Array>();

function readChunk(file: File, index: number): Uint8Array {
  const cached = readCache.get(index);
  if (cached) {
    readCache.delete(index);
    readCache.set(index, cached);
    return cached;
  }
  const start = index * READ_CHUNK_BYTES;
  const bytes = new Uint8Array(fileReader.readAsArrayBuffer(file.slice(start, Math.min(file.size, start + READ_CHUNK_BYTES))));
  perf.fileReads++;
  readCache.set(index, bytes);
  if (readCache.size > READ_CACHE_CHUNKS) {
    const oldest = readCache.keys().next();
    if (!oldest.done) readCache.delete(oldest.value);
  }
  return bytes;
}

const readGameFile: GameFileReadHook = (offset, destination, size) => {
  if (!activeGameFile || !coreMemory) return false;
  const file = activeGameFile;
  const started = performance.now();
  try {
    if (offset + size > file.size) return false;
    const target = new Uint8Array(coreMemory.buffer, destination, size);
    if (size > READ_DIRECT_BYTES) {
      const bytes = fileReader.readAsArrayBuffer(file.slice(offset, offset + size));
      perf.fileReads++;
      if (bytes.byteLength !== size) return false;
      target.set(new Uint8Array(bytes));
      return true;
    }
    let done = 0;
    while (done < size) {
      const at = offset + done;
      const index = Math.floor(at / READ_CHUNK_BYTES);
      const chunk = readChunk(file, index);
      const within = at - index * READ_CHUNK_BYTES;
      const take = Math.min(size - done, chunk.byteLength - within);
      if (take <= 0) return false;
      target.set(chunk.subarray(within, within + take), done);
      done += take;
    }
    return true;
  } catch (e) {
    log("warn", `game file read failed at offset ${offset}: ${e instanceof Error ? e.message : String(e)}`);
    return false;
  } finally {
    perf.fileReadBytes += size;
    perf.fileReadMs += performance.now() - started;
  }
};

const guestOutput: GuestOutputHook = (address, length) => {
  if (!coreMemory) return;
  // slice() copies out of the shared buffer; TextDecoder rejects SAB views.
  const bytes = new Uint8Array(coreMemory.buffer, address, length).slice();
  const msg: CPUToMainMessage = { type: "guest-output", text: new TextDecoder().decode(bytes) };
  self.postMessage(msg);
};

/* ------------------------------------------------------------------ */
/* Run loop (§7): scheduler slices in ~12ms bursts, then yield so      */
/* lifecycle messages (pause, a new load) are handled promptly.        */
/* ------------------------------------------------------------------ */

const SLICE_CYCLES = 200_000n;

/** Run-loop counters, read by the perf harness (platform/web/tools/perf.mjs)
 * through the worker's global scope; nothing posts them. */
interface CpuPerf {
  /** performance.now() when the current game started running. */
  startedMs: number;
  slices: number;
  /** Time inside emulator_run_slice (includes file reads). */
  sliceMs: number;
  /** Time between bursts: the event loop, timers, messages. */
  yieldMs: number;
  bursts: number;
  fileReads: number;
  fileReadBytes: number;
  fileReadMs: number;
  /** Core counters (emulator_perf_counters_ffi), refreshed every burst. */
  ticks: number;
  svcs: number;
  gpuBytes: number;
  gpuStalls: number;
}
const perf: CpuPerf = {
  startedMs: 0, slices: 0, sliceMs: 0, yieldMs: 0, bursts: 0,
  fileReads: 0, fileReadBytes: 0, fileReadMs: 0, ticks: 0, svcs: 0, gpuBytes: 0, gpuStalls: 0,
};
(globalThis as unknown as { __VOLAND_CPU_PERF__: CpuPerf }).__VOLAND_CPU_PERF__ = perf;
const PERF_COUNTERS = 4;
let perfBuffer = 0;
let lastBurstEnd = 0;

/* Yielding between bursts: setTimeout(0) is clamped to 4 ms once timers
 * nest (HTML timer rules), which cost a quarter of the worker's time
 * against 12 ms bursts. A message to ourselves on a private channel runs
 * as the next task with no clamp, and still lets lifecycle messages and
 * timers in between. It carries no data (§6: a scheduling signal only). */
const yieldChannel = new MessageChannel();
/** One burst at a time, however often load/resume ask for one. */
let burstPending = false;
yieldChannel.port1.onmessage = () => {
  burstPending = false;
  runBurst();
};
function scheduleBurst(): void {
  if (burstPending) return;
  burstPending = true;
  yieldChannel.port2.postMessage(null);
}

function refreshCorePerf(): void {
  if (!core || !coreMemory) return;
  if (perfBuffer === 0) perfBuffer = core._malloc(PERF_COUNTERS * 8);
  if (perfBuffer === 0) return;
  const count = core._emulator_perf_counters_ffi(BigInt(perfBuffer), PERF_COUNTERS);
  const values = new BigUint64Array(coreMemory.buffer, perfBuffer, PERF_COUNTERS);
  if (count >= PERF_COUNTERS) {
    perf.ticks = Number(values[0] ?? 0n);
    perf.svcs = Number(values[1] ?? 0n);
    perf.gpuBytes = Number(values[2] ?? 0n);
    perf.gpuStalls = Number(values[3] ?? 0n);
  }
}
const BURST_MS = 12;
let running = false;
let paused = false;
/** The user's frame skip (set-frame-skip); applied to every core load. */
let frameSkip = 0;
/** The renderer the main thread chose (set-gpu-mode); applied to every core load. */
let gpuMode = false;

function postRunState(state: "running" | "exited" | "crashed" | "deadlock" | "paused", detail: string): void {
  const msg: CPUToMainMessage = { type: "run-state", state, detail };
  self.postMessage(msg);
}

/* A software keyboard is up (the guest is waiting on it): ask the player
 * once per request. */
const TEXT_REQUEST_BYTES = 8192;
let textRequestShown = false;

function pollTextInput(): void {
  if (!core || !coreMemory || textRequestShown) return;
  const buffer = core._malloc(TEXT_REQUEST_BYTES);
  if (buffer === 0) return;
  try {
    if (core._emulator_text_request_ffi(BigInt(buffer), TEXT_REQUEST_BYTES) !== 1) return;
    const bytes = new Uint8Array(coreMemory.buffer, buffer, TEXT_REQUEST_BYTES).slice();
    const end = bytes.indexOf(0);
    const request = parseTextInputRequest(new TextDecoder().decode(bytes.subarray(0, end < 0 ? bytes.length : end)));
    if (!request) return;
    textRequestShown = true;
    const msg: CPUToMainMessage = { type: "text-input-request", request };
    self.postMessage(msg);
  } finally {
    core._free(buffer);
  }
}

function runBurst(): void {
  if (!core || !running || paused) return;
  const burstStart = performance.now();
  if (lastBurstEnd > 0) perf.yieldMs += burstStart - lastBurstEnd;
  perf.bursts++;
  const deadline = burstStart + BURST_MS;
  let now = burstStart;
  while (now < deadline) {
    const status = core._emulator_run_slice_ffi(SLICE_CYCLES);
    const after = performance.now();
    perf.slices++;
    perf.sliceMs += after - now;
    now = after;
    const finished = runStateAfterSlice(status);
    if (finished) {
      running = false;
      const ticks = core._emulator_virtual_ticks_ffi();
      const detail = finished === "crashed"
        ? `stopped at pc=0x${core._emulator_crash_pc_ffi().toString(16)}`
        : `virtual time ${ticks} ticks`;
      postRunState(finished, detail);
      return;
    }
  }
  pollTextInput();
  refreshCorePerf();
  lastBurstEnd = performance.now();
  scheduleBurst();
}

function startRunning(): void {
  Object.assign(perf, { startedMs: performance.now(), slices: 0, sliceMs: 0, yieldMs: 0, bursts: 0,
                        fileReads: 0, fileReadBytes: 0, fileReadMs: 0 });
  lastBurstEnd = 0;
  running = true;
  paused = false;
  postRunState("running", "");
  scheduleBurst();
}

async function loadCoreModule(memory: WebAssembly.Memory): Promise<SwitchCoreExports> {
  /* An absolute URL, not the bare "/core/..." path: Vite's dev server
   * appends `?import` to root-relative dynamic imports and then refuses
   * to serve files under public/ as modules ("can only be referenced via
   * HTML tags"). A full URL bypasses that rewrite in dev and is
   * equivalent in a production build. */
  const coreUrl = new URL(CORE_MODULE_URL, self.location.origin).href;
  const factory = (await import(/* @vite-ignore */ coreUrl)).default as CoreModuleFactory;
  return factory({ wasmMemory: memory, volandReadGameFile: readGameFile, volandGuestOutput: guestOutput });
}

function randomAslrSeed(): bigint {
  const words = crypto.getRandomValues(new BigUint64Array(1));
  const seed = words[0] ?? 1n;
  return seed === 0n ? 1n : seed; // 0 disables ASLR (address_space.h)
}

/** Copies `bytes` into a fresh `_malloc` block; the caller owns it. */
function copyIntoCore(bytes: Uint8Array): number {
  if (!core || !coreMemory) return 0;
  const pointer = core._malloc(bytes.byteLength + 1);
  if (pointer === 0) return 0;
  const view = new Uint8Array(coreMemory.buffer, pointer, bytes.byteLength + 1);
  view.set(bytes);
  view[bytes.byteLength] = 0;
  return pointer;
}

/** Runs `fn` with `text` as a NUL-terminated C string in core memory. */
function withCString(text: string, fn: (pointer: bigint) => void): void {
  if (!core) return;
  const pointer = copyIntoCore(new TextEncoder().encode(text));
  if (pointer === 0) return;
  try {
    fn(BigInt(pointer));
  } finally {
    core._free(pointer);
  }
}

/** SD-card file name for a host file: path separators and control
 * characters are not allowed in a single component. */
function sdName(name: string): string {
  const cleaned = name.replace(/[\/\\:*?"<>|\u0000-\u001f]/g, "_");
  return cleaned.length > 0 ? cleaned : "program.nro";
}

async function loadSharedFont(): Promise<void> {
  if (!core) return;
  try {
    const response = await fetch(SHARED_FONT_URL);
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const bytes = new Uint8Array(await response.arrayBuffer());
    const pointer = copyIntoCore(bytes); // never freed: the core keeps using it
    if (pointer !== 0) core._emulator_set_shared_font_ffi(BigInt(pointer), bytes.byteLength);
  } catch (e) {
    log("warn", `no shared font (${SHARED_FONT_URL}): ${e instanceof Error ? e.message : String(e)}`);
  }
}

/* Largest file copied onto the SD card (it lives in core memory). */
const SD_MAX_FILE_BYTES = 64 * 1024 * 1024;

/** Writes `bytes` to the core's SD card at `path`. */
function writeSdFile(path: string, bytes: Uint8Array): boolean {
  if (!core || bytes.byteLength > SD_MAX_FILE_BYTES) return false;
  const sdCore = core;
  const data = copyIntoCore(bytes);
  if (data === 0) return false;
  let ok = false;
  withCString(path, (pathPointer) => {
    ok = sdCore._emulator_sd_write_file_ffi(pathPointer, BigInt(data), BigInt(bytes.byteLength)) === CoreResult.Ok;
  });
  sdCore._free(data);
  return ok;
}

async function addToSdCard(files: readonly File[]): Promise<CPUToMainMessage> {
  const added: string[] = [];
  const failed: string[] = [];
  for (const file of files) {
    const name = sdName(file.name);
    const path = name.toLowerCase().endsWith(".nro") ? `/switch/${name}` : `/${name}`;
    let ok = false;
    try {
      const bytes = new Uint8Array(fileReader.readAsArrayBuffer(file));
      ok = writeSdFile(path, bytes);
      if (ok && !(await persistSdFile(path, bytes))) log("warn", `SD card: ${path} will not survive a reload (no OPFS)`);
    } catch (e) {
      log("warn", `SD card: ${file.name}: ${e instanceof Error ? e.message : String(e)}`);
    }
    (ok ? added : failed).push(ok ? path : file.name);
  }
  return { type: "sd-files-added", added, failed };
}

/* ------------------------------------------------------------------ */
/* Guest writes -> OPFS (§15): every SD_MIRROR_MS, if any filesystem    */
/* changed, diff the SD manifest and store / remove what changed.      */
/* ------------------------------------------------------------------ */

const SD_MIRROR_MS = 3000;
let mirrorGeneration = -1;
let mirrorManifest: ReadonlyMap<string, string> = new Map();
let mirroring = false;

function readSdManifest(): ReadonlyMap<string, string> | null {
  if (!core) return null;
  const need = core._emulator_sd_manifest_ffi(0n, 0);
  const buffer = core._malloc(need + 1);
  if (buffer === 0) return null;
  try {
    const written = core._emulator_sd_manifest_ffi(BigInt(buffer), need + 1);
    if (!coreMemory || written > need + 1) return null;
    const text = new TextDecoder().decode(new Uint8Array(coreMemory.buffer, buffer, written).slice());
    return parseManifest(text);
  } finally {
    core._free(buffer);
  }
}

function readSdFile(path: string): Uint8Array | null {
  if (!core || !coreMemory) return null;
  const sdCore = core;
  let bytes: Uint8Array | null = null;
  withCString(path, (pathPointer) => {
    const size = sdCore._emulator_sd_read_file_ffi(pathPointer, 0n, 0);
    if (size < 0 || size > SD_MAX_FILE_BYTES) return;
    const buffer = sdCore._malloc(size || 1);
    if (buffer === 0 || !coreMemory) return;
    const read = sdCore._emulator_sd_read_file_ffi(pathPointer, BigInt(buffer), size);
    if (read === size) bytes = new Uint8Array(coreMemory.buffer, buffer, size).slice();
    sdCore._free(buffer);
  });
  return bytes;
}

/** Records the current SD contents as already stored (after a restore). */
function baselineSdMirror(): void {
  if (!core) return;
  mirrorManifest = readSdManifest() ?? new Map();
  mirrorGeneration = core._emulator_sd_generation_ffi();
}

async function mirrorSdChanges(): Promise<void> {
  if (!core || mirroring || mirrorGeneration < 0) return;
  const generation = core._emulator_sd_generation_ffi();
  if (generation === mirrorGeneration) return;
  mirroring = true;
  try {
    const after = readSdManifest();
    if (!after) return;
    const { changed, removed } = diffManifests(mirrorManifest, after);
    for (const path of changed) {
      const bytes = readSdFile(path);
      if (bytes) await persistSdFile(path, bytes);
    }
    for (const path of removed) await removeSdFile(path);
    mirrorManifest = after;
    mirrorGeneration = generation;
  } catch (e) {
    log("warn", `SD card: could not store guest changes: ${e instanceof Error ? e.message : String(e)}`);
  } finally {
    mirroring = false;
  }
}

async function clearSdCard(): Promise<CPUToMainMessage> {
  if (core && core._emulator_sd_clear_ffi() !== CoreResult.Ok) log("warn", "SD card: a file is in use; reload to clear it");
  await clearSdFiles();
  baselineSdMirror();
  return { type: "sd-files-added", added: [], failed: [] };
}

function loadGame(file: File): CPUToMainMessage {
  if (!core || !coreMemory) {
    return { type: "load-failed", failure: { reason: "internal", message: "the emulator core is not loaded" } };
  }
  log("info", `loading ${file.name} (${file.size} bytes)`);

  /* One program per Emulator (emulator.h): replace whatever was loaded. */
  running = false;
  core._emulator_unload_program_ffi();

  activeGameFile = file;
  readCache.clear();
  const loadingCore = core;
  loadingCore._emulator_set_rtc_ffi(BigInt(Math.floor(Date.now() / 1000)));
  loadingCore._emulator_set_frame_skip_ffi(frameSkip);
  loadingCore._emulator_set_gpu_mode_ffi(gpuMode ? 1 : 0);
  withCString(`/${sdName(file.name)}`, (path) => loadingCore._emulator_set_program_path_ffi(path));
  const code = core._emulator_load_program_ffi(BigInt(file.size), randomAslrSeed());

  if (code !== CoreResult.Ok) {
    activeGameFile = null;
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

  await loadSharedFont();
  try {
    const restored = await restoreSdFiles(writeSdFile);
    if (restored > 0) log("info", `SD card: restored ${restored} file(s) from browser storage`);
  } catch (e) {
    log("warn", `SD card: could not restore stored files: ${e instanceof Error ? e.message : String(e)}`);
  }
  baselineSdMirror();
  setInterval(() => void mirrorSdChanges(), SD_MIRROR_MS);

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
    const outcome = loadGame(msg.file);
    self.postMessage(outcome);
    if (outcome.type === "game-loaded") startRunning();
    return;
  }

  if (msg.type === "sd-add-files") {
    void addToSdCard(msg.files).then((reply) => self.postMessage(reply));
    return;
  }
  if (msg.type === "sd-clear") {
    void clearSdCard().then((reply) => self.postMessage(reply));
    return;
  }

  if (msg.type === "controller-connected") {
    log("info", `controller connected in slot ${msg.index} (profile ${msg.profileId})`);
    return;
  }
  if (msg.type === "controller-disconnected") {
    log("info", `controller disconnected from slot ${msg.index}`);
    return;
  }

  if (msg.type === "text-input-response") {
    if (core) {
      const sdCore = core;
      withCString(msg.text, (pointer) => sdCore._emulator_text_respond_ffi(pointer, msg.accepted ? 1 : 0));
    }
    textRequestShown = false;
    return;
  }

  if (msg.type === "set-gpu-mode") {
    gpuMode = msg.on;
    core?._emulator_set_gpu_mode_ffi(gpuMode ? 1 : 0);
    log("info", `renderer: ${gpuMode ? "WebGPU (GPU worker)" : "software reference"}`);
    return;
  }

  if (msg.type === "set-frame-skip") {
    frameSkip = msg.frames;
    core?._emulator_set_frame_skip_ffi(frameSkip);
    return;
  }

  if (msg.type === "pause") {
    if (running && !paused) postRunState("paused", "");
    paused = true;
    return;
  }
  if (msg.type === "resume") {
    if (paused && running) {
      paused = false;
      postRunState("running", "");
      scheduleBurst();
    }
    paused = false;
    return;
  }

  if (msg.type === "halt") {
    const halted: CPUToMainMessage = { type: "halted" };
    self.postMessage(halted);
    return;
  }
});
