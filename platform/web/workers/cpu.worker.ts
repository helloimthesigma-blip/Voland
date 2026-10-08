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
import { isSaveName, loadSaveArchives, readTar, removeSaveArchive, saveNameOfTar, storeSaveArchive, tarNameOfSave, writeTar } from "./save-store";
import { writeSaveArchive } from "@bindings/save-archive";
import {
  PLAN_BYTES, createStateFile, deleteState, listStates, newStateId, openStateFile, parsePlan, planMismatch,
  readState, readStoredPlan, stateInfo, writeState, writeStateInfo,
} from "./savestate";
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

/* Under the app's base path (vite.config.ts `base`), so a build served
 * from a subdirectory (e.g. a GitHub Pages project site) finds its files. */
const CORE_MODULE_URL = `${import.meta.env.BASE_URL}core/switch_core.js`;

/* pl:u's system font (§1.6: an openly licensed font - Noto Sans, SIL OFL
 * 1.1, see public/fonts/OFL.txt - never Nintendo's). */
const SHARED_FONT_URL = `${import.meta.env.BASE_URL}fonts/NotoSans-Regular.ttf`;

/** Upper bound on a core Error message (static strings, all short). */
const CORE_ERROR_MESSAGE_MAX_BYTES = 1024;
/** core/common/log.h LOG_RECENT_LINES x (LOG_RECENT_LINE_BYTES + 1). */
const RECENT_PROBLEMS_MAX_BYTES = 32 * 241;

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
const READ_CHUNK_BYTES = 256 * 1024;
const READ_CACHE_CHUNKS = 512;
/** Reads bigger than this skip the cache (one direct read). */
const READ_DIRECT_BYTES = 4 * 1024 * 1024;
/** Chunk index -> bytes, in least- to most-recently used order. */
const readCache = new Map<number, Uint8Array>();

/** Chunks read at least once since the load (read amplification). */
const chunksSeen = new Set<number>();

function cacheChunk(index: number, bytes: Uint8Array): void {
  readCache.delete(index);
  readCache.set(index, bytes);
  if (readCache.size > READ_CACHE_CHUNKS) {
    const oldest = readCache.keys().next();
    if (!oldest.done) readCache.delete(oldest.value);
  }
}

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
  chunksSeen.add(index);
  perf.fileChunks = chunksSeen.size;
  cacheChunk(index, bytes);
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
  /** Distinct chunks read (more reads than this are re-reads of evicted chunks). */
  fileChunks: number;
  /** Core counters (emulator_perf_counters_ffi), refreshed every burst. */
  ticks: number;
  svcs: number;
  gpuBytes: number;
  gpuStalls: number;
  /** Time the core waited for the GPU/video worker to drain a full ring. */
  streamWaitMs: number;
}
const perf: CpuPerf = {
  startedMs: 0, slices: 0, sliceMs: 0, yieldMs: 0, bursts: 0,
  fileReads: 0, fileReadBytes: 0, fileReadMs: 0, fileChunks: 0, ticks: 0, svcs: 0, gpuBytes: 0, gpuStalls: 0, streamWaitMs: 0,
};
(globalThis as unknown as { __VOLAND_CPU_PERF__: CpuPerf }).__VOLAND_CPU_PERF__ = perf;
const PERF_COUNTERS = 5;
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
    perf.streamWaitMs = Number(values[4] ?? 0n) / 1e6;
  }
}
const BURST_MS = 12;
let running = false;
let loadedTitleId: string | null = null;
let paused = false;
/** The user's frame skip (set-frame-skip); applied to every core load. */
let frameSkip = 0;
/** The renderer the main thread chose (set-gpu-mode); applied to every core load. */
let gpuMode = false;
/** Host threads running guest threads (docs/PARALLEL.md): the Switch's
 * three application cores where the machine has room for them besides the
 * page and the CPU and GPU workers (Silksong gameplay: 1.94x parallelism
 * on 3, 1.35x on 2). ?cores=N (set-host-cores) overrides; 0 is the serial
 * scheduler. Applied to every core load. */
const MAX_DEFAULT_HOST_CORES = 3;
const HOST_THREADS_RESERVED = 2;
const DEFAULT_HOST_CORES = Math.max(
  1, Math.min(MAX_DEFAULT_HOST_CORES, (self.navigator.hardwareConcurrency || 4) - HOST_THREADS_RESERVED));
/** Cores chosen with ?cores=N (set-host-cores); null = the default. */
let requestedHostCores: number | null = null;
/** Titles that run faster on the serial scheduler, where the default picks
 * it: their guest threads hand work to each other so tightly that, with
 * the GPU thread beside them, parallel cores mostly wait (Super Smash
 * Bros. Ultimate fight: 18-22 fps serial, 9.5 on three cores). */
const SERIAL_TITLES: ReadonlySet<string> = new Set(["01006A800016E000"]);


/** Poll coalescing (set-poll-coalescing; ?polls=0 turns it off). */
let pollCoalescing = true;
/** Free-running guest cores (set-free-running; ?free=N): from slice N on,
 * one run_for call per burst instead of a loop of slices. 0 = off. */
let freeFromSlice = 0;
/** Wall-clock pacing (set-pacing; ?pacing=0 turns it off): the game runs at
 * real speed with fewer frames instead of in slow motion. */
let pacing = true;
let freeRunning = false;
/** Asynchronous GPU (set-gpu-async; ?gpuasync=0 turns it off): GPU command
 * processing on its own thread beside the guest (docs/ASYNC_GPU.md). */
let gpuAsync = true;
/** The renderer's threads (set-render-workers; ?rworkers=N): 0 = as many as it starts with. */
let renderWorkers = 0;

function applyGpuAsync(target: SwitchCoreExports): void {
  const on = target._emulator_set_gpu_async_ffi(gpuAsync ? 1 : 0) === 1;
  log("info", `asynchronous GPU ${on ? "on" : gpuAsync ? "unavailable" : "off"}`);
}

function applyHostCores(target: SwitchCoreExports): void {
  const serialTitle = loadedTitleId !== null && SERIAL_TITLES.has(loadedTitleId);
  const hostCores = requestedHostCores ?? (serialTitle ? 0 : DEFAULT_HOST_CORES);
  const inEffect = target._emulator_set_host_cores_ffi(hostCores);
  log("info", `guest threads on ${inEffect === 0 ? "the serial scheduler" : `${inEffect} host core(s)`}`);
}

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
    if (!freeRunning && freeFromSlice > 0 && perf.slices >= freeFromSlice - 1) {
      freeRunning = true;
      core._emulator_set_free_running_ffi(1);
      log("info", `free-running guest cores from slice ${perf.slices + 1}`);
    }
    const status = freeRunning
      ? core._emulator_run_for_ffi(Math.max(1, Math.ceil(deadline - now)), SLICE_CYCLES)
      : core._emulator_run_slice_ffi(SLICE_CYCLES);
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
      if (finished !== "exited") {
        /* The core's own recent warnings/errors (e.g. svcBreak's reason),
         * which otherwise only reach the developer console. */
        const recent = readCString((coreMemory as WebAssembly.Memory).buffer, Number(core._emulator_recent_problems_ffi()), RECENT_PROBLEMS_MAX_BYTES);
        for (const line of recent.split("\n").filter((l) => l.length > 0)) log("warn", `core: ${line}`);
      }
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
                        fileReads: 0, fileReadBytes: 0, fileReadMs: 0, fileChunks: 0 });
  lastBurstEnd = 0;
  running = true;
  paused = false;
  core?._emulator_pacing_resync_ffi();
  postRunState("running", "");
  scheduleBurst();
}

async function loadCoreModule(memory: WebAssembly.Memory): Promise<SwitchCoreExports> {
  /* An absolute URL, not the bare root-relative path: Vite's dev server
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
    const diff = diffManifests(mirrorManifest, after);
    /* Saves persist when the game commits them (mirrorCommittedSaves),
     * whole; the live save trees are not mirrored file by file. */
    const changed = diff.changed.filter((path) => !path.startsWith(SAVE_PATH_PREFIX));
    const removed = diff.removed.filter((path) => !path.startsWith(SAVE_PATH_PREFIX));
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

/* ------------------------------------------------------------------ */
/* Committed saves -> OPFS (§15, save-store.ts): whenever the game      */
/* commits a save, its whole archive is stored, atomically.            */
/* ------------------------------------------------------------------ */

const SAVE_PATH_PREFIX = "save:";
const COMMIT_PATH_PREFIX = "commit:/";
const SAVE_COMMIT_POLL_MS = 250;
let savedCommits = -1;
let savedArchives: ReadonlyMap<string, string> = new Map();
let savingCommits = false;

function readCommittedManifest(): ReadonlyMap<string, string> | null {
  if (!core || !coreMemory) return null;
  const need = core._emulator_save_committed_manifest_ffi(0n, 0);
  const buffer = core._malloc(need + 1);
  if (buffer === 0) return null;
  try {
    const written = core._emulator_save_committed_manifest_ffi(BigInt(buffer), need + 1);
    if (written > need + 1) return null;
    return parseManifest(new TextDecoder().decode(new Uint8Array(coreMemory.buffer, buffer, written).slice()));
  } finally {
    core._free(buffer);
  }
}

/** Records what the core holds as already stored (after a restore). */
function baselineCommittedSaves(): void {
  if (!core) return;
  savedArchives = readCommittedManifest() ?? new Map();
  savedCommits = core._emulator_save_commits_ffi();
}

async function mirrorCommittedSaves(): Promise<void> {
  if (!core || savingCommits || savedCommits < 0) return;
  const commits = core._emulator_save_commits_ffi();
  if (commits === savedCommits) return;
  savingCommits = true;
  try {
    const after = readCommittedManifest();
    if (!after) return;
    for (const [path, key] of after) {
      if (savedArchives.get(path) === key || !path.startsWith(COMMIT_PATH_PREFIX)) continue;
      const bytes = readSdFile(path);
      const name = path.slice(COMMIT_PATH_PREFIX.length);
      if (bytes && !(await storeSaveArchive(name, bytes))) log("warn", "saves: no browser storage - progress will not survive a reload");
    }
    /* A save the core renamed (a title taking over an unkeyed save). */
    for (const path of savedArchives.keys()) {
      if (path.startsWith(COMMIT_PATH_PREFIX) && !after.has(path)) await removeSaveArchive(path.slice(COMMIT_PATH_PREFIX.length));
    }
    savedArchives = after;
    savedCommits = commits;
  } catch (e) {
    log("warn", `saves: could not store a commit: ${e instanceof Error ? e.message : String(e)}`);
  } finally {
    savingCommits = false;
  }
}

/** Loads one archive into the core's save `name`. */
function restoreSaveArchive(name: string, bytes: Uint8Array): boolean {
  if (!core || !coreMemory) return false;
  const saveCore = core;
  let ok = false;
  withCString(name, (namePointer) => {
    const buffer = saveCore._malloc(bytes.byteLength || 1);
    if (buffer === 0 || !coreMemory) return;
    new Uint8Array(coreMemory.buffer, buffer, bytes.byteLength).set(bytes);
    ok = saveCore._emulator_save_restore_archive_ffi(namePointer, BigInt(buffer), bytes.byteLength) === CoreResult.Ok;
    saveCore._free(buffer);
  });
  return ok;
}

async function restoreSaves(): Promise<void> {
  const archives = await loadSaveArchives();
  let restored = 0;
  for (const [name, bytes] of archives) if (restoreSaveArchive(name, bytes)) restored++;
  if (restored > 0) log("info", `saves: restored ${restored} game save(s) from browser storage`);
  if (restored < archives.size) log("warn", `saves: ${archives.size - restored} stored save(s) could not be loaded`);
}

async function exportSaves(): Promise<CPUToMainMessage> {
  await mirrorCommittedSaves(); /* the latest commit first */
  const archives = await loadSaveArchives();
  const files = new Map<string, Uint8Array>();
  for (const [name, bytes] of archives) files.set(tarNameOfSave(name), bytes);
  const tar = writeTar(files);
  return { type: "saves-exported", tar: tar.buffer as ArrayBuffer, count: archives.size };
}

async function importSaves(tarBytes: ArrayBuffer): Promise<CPUToMainMessage> {
  let imported = 0;
  let rejected = 0;
  for (const [entry, bytes] of readTar(new Uint8Array(tarBytes))) {
    const name = saveNameOfTar(entry);
    if (!name) continue;
    /* Into the core first: it validates the archive; only good ones are stored. */
    if (restoreSaveArchive(name, bytes) && (await storeSaveArchive(name, bytes))) imported++;
    else rejected++;
  }
  baselineCommittedSaves();
  log("info", `saves: imported ${imported}${rejected ? `, ${rejected} rejected (damaged, or in use by the running game)` : ""}`);
  return { type: "saves-imported", imported, rejected };
}

/* The save browser: list, replace, delete (src/ui/SavesPanel.tsx). */
async function listSaves(): Promise<CPUToMainMessage> {
  await mirrorCommittedSaves(); /* the latest commit first */
  const archives = await loadSaveArchives();
  const saves = [...archives].map(([name, bytes]) => ({ name, archive: bytes.slice().buffer as ArrayBuffer }));
  return { type: "saves-listed", saves };
}

/** Replaces save `name` with `archive`: into the core first (which
 * validates it, and refuses while the running game has a file of the
 * save open), then into storage. */
async function putSave(name: string, archive: ArrayBuffer): Promise<CPUToMainMessage> {
  const bytes = new Uint8Array(archive);
  const ok = isSaveName(name) && restoreSaveArchive(name, bytes) && (await storeSaveArchive(name, bytes));
  if (ok) baselineCommittedSaves();
  log(ok ? "info" : "warn", ok ? `saves: ${name.slice(0, 24)}… updated` : "saves: could not update a save (in use by the running game?)");
  return { type: "save-put", name, ok };
}

/** Deletes save `name`: the running session sees it empty, and it is no
 * longer stored, so it is gone after a reload. */
async function deleteSave(name: string): Promise<CPUToMainMessage> {
  const ok = isSaveName(name) && restoreSaveArchive(name, writeSaveArchive(new Map()));
  if (ok) {
    await removeSaveArchive(name);
    baselineCommittedSaves();
  }
  return { type: "save-deleted", name, ok };
}

/* ------------------------------------------------------------------ */
/* Save states (workers/savestate.ts): the machine frozen between       */
/* slices, its ranges copied synchronously.                             */
/* ------------------------------------------------------------------ */

let stateBusy = false;

const RENDER_STAT_NAMES = [
  "draws", "skippedDraws", "shaderFaults", "unknownShaderOps", "textureMisses",
  "gpuDraws", "untranslatedGpuDraws", "translatedShaders", "textureUploads", "presents",
] as const;
const DISTINCT_PROBLEMS_MAX_BYTES = 256 * 1024;
const TIMER_HZ = 19_200_000;

function compatReport(): CPUToMainMessage {
  if (!core || !coreMemory) {
    return { type: "compat-report", report: { titleId: null, backend: "", slices: 0, virtualSeconds: 0, wallSeconds: 0, problems: "", render: {} } };
  }
  const buffer = coreMemory.buffer;
  const problems = readCString(buffer, Number(core._emulator_distinct_problems_ffi()), DISTINCT_PROBLEMS_MAX_BYTES);
  const stats = new BigUint64Array(buffer, Number(core._emulator_render_stats_ffi()), RENDER_STAT_NAMES.length);
  const render = Object.fromEntries(RENDER_STAT_NAMES.map((name, i) => [name, Number(stats[i] ?? 0n)]));
  return {
    type: "compat-report",
    report: {
      titleId: loadedTitleId,
      backend: CPU_BACKEND_DISPLAY_NAMES[core._cpu_backend_id_ffi() as CpuBackendId] ?? "unknown",
      slices: perf.slices,
      virtualSeconds: Number(core._emulator_virtual_ticks_ffi()) / TIMER_HZ,
      wallSeconds: perf.startedMs ? (performance.now() - perf.startedMs) / 1000 : 0,
      problems,
      render,
    },
  };
}

function corePlan(address: number): ReturnType<typeof parsePlan> | null {
  if (!coreMemory || address === 0) return null;
  return parsePlan(new Uint8Array(coreMemory.buffer, address, PLAN_BYTES).slice());
}

function coreErrorMessage(): string {
  if (!core || !coreMemory) return "the core is not running";
  return readCString(coreMemory.buffer, Number(core._emulator_last_error_message_ffi()), CORE_ERROR_MESSAGE_MAX_BYTES);
}

async function saveState(): Promise<CPUToMainMessage> {
  const titleId = loadedTitleId;
  if (!core || !coreMemory || !titleId) return { type: "state-saved", info: null, error: "no game is running" };
  if (stateBusy) return { type: "state-saved", info: null, error: "another save state is in progress" };
  stateBusy = true;
  const createdAt = Date.now();
  const id = newStateId(titleId, createdAt);
  let file: FileSystemSyncAccessHandle | null = null;
  try {
    file = await createStateFile(id); /* before freezing: opening is asynchronous */
    const started = performance.now();
    const plan = corePlan(core._emulator_savestate_begin_save_ffi());
    if (!plan) throw new Error(coreErrorMessage());
    let bytes = 0;
    try {
      bytes = writeState(file, coreMemory.buffer, plan);
    } finally {
      core._emulator_savestate_end_save_ffi();
      core._emulator_pacing_resync_ffi(); /* the frozen moment does not count */
    }
    file.close();
    file = null;
    const info = stateInfo(id, titleId, plan, bytes, createdAt);
    await writeStateInfo(info);
    log("info", `save state: ${(bytes / 2 ** 20).toFixed(0)} MiB in ${((performance.now() - started) / 1000).toFixed(1)} s`);
    return { type: "state-saved", info, error: "" };
  } catch (e) {
    file?.close();
    await deleteState(id);
    const error = e instanceof Error ? e.message : String(e);
    log("warn", `save state failed: ${error}`);
    return { type: "state-saved", info: null, error };
  } finally {
    stateBusy = false;
  }
}

async function loadState(id: string): Promise<CPUToMainMessage> {
  if (!core || !coreMemory || !loadedTitleId) return { type: "state-loaded", ok: false, error: "start the game first, then load its state" };
  if (stateBusy) return { type: "state-loaded", ok: false, error: "another save state is in progress" };
  stateBusy = true;
  let file: FileSystemSyncAccessHandle | null = null;
  try {
    file = await openStateFile(id);
    const stored = readStoredPlan(file);
    if (!stored) return { type: "state-loaded", ok: false, error: "the state file is damaged" };
    const started = performance.now();
    const now = corePlan(core._emulator_savestate_begin_restore_ffi());
    if (!now) return { type: "state-loaded", ok: false, error: coreErrorMessage() };
    const mismatch = planMismatch(stored, now);
    if (mismatch) {
      core._emulator_savestate_finish_restore_ffi(0);
      return { type: "state-loaded", ok: false, error: `This state cannot be loaded: ${mismatch}.` };
    }
    if (!readState(file, coreMemory.buffer, stored, now)) {
      /* Memory is half written: the running game cannot continue as it was. */
      core._emulator_savestate_finish_restore_ffi(0);
      running = false;
      postRunState("crashed", "a save state could not be read completely; reload the page");
      return { type: "state-loaded", ok: false, error: "the state file is truncated" };
    }
    const code = core._emulator_savestate_finish_restore_ffi(1);
    core._emulator_pacing_resync_ffi();
    /* Rolled-back SD card and saves: the mirrors start from here. */
    baselineCommittedSaves();
    baselineSdMirror();
    log("info", `save state loaded in ${((performance.now() - started) / 1000).toFixed(1)} s`);
    if (code !== CoreResult.Ok) log("warn", `save state: ${coreErrorMessage()}`);
    if (!running) startRunning(); /* e.g. back from a crash */
    else if (!paused) scheduleBurst();
    return { type: "state-loaded", ok: true, error: "" };
  } catch (e) {
    return { type: "state-loaded", ok: false, error: e instanceof Error ? e.message : String(e) };
  } finally {
    file?.close();
    stateBusy = false;
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
  chunksSeen.clear();
  const loadingCore = core;
  loadingCore._emulator_set_rtc_ffi(BigInt(Math.floor(Date.now() / 1000)));
  loadingCore._emulator_set_frame_skip_ffi(frameSkip);
  loadingCore._emulator_set_gpu_mode_ffi(gpuMode ? 1 : 0);
  applyGpuAsync(loadingCore);
  applyHostCores(loadingCore);
  if (renderWorkers > 0) log("info", `renderer workers: ${loadingCore._emulator_set_render_workers_ffi(renderWorkers)}`);
  loadingCore._emulator_set_poll_coalescing_ffi(pollCoalescing ? 1 : 0);
  freeRunning = false;
  loadingCore._emulator_set_free_running_ffi(0);
  loadingCore._emulator_set_pacing_ffi(pacing ? 1 : 0);
  withCString(`/${sdName(file.name)}`, (path) => loadingCore._emulator_set_program_path_ffi(path));
  const code = core._emulator_load_program_ffi(BigInt(file.size), randomAslrSeed());

  if (code !== CoreResult.Ok) {
    activeGameFile = null;
    const message = readCString(coreMemory.buffer, Number(core._emulator_last_error_message_ffi()),
                                CORE_ERROR_MESSAGE_MAX_BYTES);
    return { type: "load-failed", failure: loadFailureFromResult(code, message) };
  }

  loadedTitleId = formatTitleId(core._emulator_program_id_ffi());
  if (requestedHostCores === null) applyHostCores(core); /* the default may depend on the title */
  return {
    type: "game-loaded",
    titleId: loadedTitleId,
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
  try {
    await restoreSaves();
  } catch (e) {
    log("warn", `saves: could not restore stored saves: ${e instanceof Error ? e.message : String(e)}`);
  }
  baselineSdMirror();
  baselineCommittedSaves();
  setInterval(() => void mirrorSdChanges(), SD_MIRROR_MS);
  setInterval(() => void mirrorCommittedSaves(), SAVE_COMMIT_POLL_MS);

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
  if (msg.type === "export-saves") {
    void exportSaves().then((reply) => self.postMessage(reply, reply.type === "saves-exported" ? [reply.tar] : []));
    return;
  }
  if (msg.type === "import-saves") {
    void importSaves(msg.tar).then((reply) => self.postMessage(reply));
    return;
  }
  if (msg.type === "compat-report") {
    self.postMessage(compatReport());
    return;
  }
  if (msg.type === "save-state") {
    void saveState().then((reply) => self.postMessage(reply));
    return;
  }
  if (msg.type === "load-state") {
    void loadState(msg.id).then((reply) => self.postMessage(reply));
    return;
  }
  if (msg.type === "list-states") {
    void listStates().then((states) => self.postMessage({ type: "states-listed", states } satisfies CPUToMainMessage))
      .catch(() => self.postMessage({ type: "states-listed", states: [] } satisfies CPUToMainMessage));
    return;
  }
  if (msg.type === "delete-state") {
    void deleteState(msg.id).then(() => self.postMessage({ type: "state-deleted", id: msg.id } satisfies CPUToMainMessage));
    return;
  }
  if (msg.type === "list-saves") {
    void listSaves().then((reply) => self.postMessage(reply, reply.type === "saves-listed" ? reply.saves.map((s) => s.archive) : []));
    return;
  }
  if (msg.type === "put-save") {
    void putSave(msg.name, msg.archive).then((reply) => self.postMessage(reply));
    return;
  }
  if (msg.type === "delete-save") {
    void deleteSave(msg.name).then((reply) => self.postMessage(reply));
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

  if (msg.type === "set-pacing") {
    pacing = msg.on;
    core?._emulator_set_pacing_ffi(pacing ? 1 : 0);
    log("info", `wall-clock pacing ${pacing ? "on" : "off"}`);
    return;
  }

  if (msg.type === "set-free-running") {
    freeFromSlice = msg.fromSlice;
    log("info", freeFromSlice > 0 ? `free-running guest cores from slice ${freeFromSlice}` : "free-running guest cores off");
    return;
  }

  if (msg.type === "set-poll-coalescing") {
    pollCoalescing = msg.on;
    core?._emulator_set_poll_coalescing_ffi(pollCoalescing ? 1 : 0);
    log("info", `poll coalescing ${pollCoalescing ? "on" : "off"}`);
    return;
  }

  if (msg.type === "set-host-cores") {
    requestedHostCores = msg.cores < 0 ? null : msg.cores; /* negative: back to the default */
    /* Takes effect between slices; a running game switches at once. */
    if (core) applyHostCores(core);
    return;
  }

  if (msg.type === "dump-threads") {
    core?._emulator_dump_threads_ffi(msg.backtrace ? 1 : 0);
    return;
  }

  if (msg.type === "set-render-workers") {
    renderWorkers = msg.count;
    if (core && renderWorkers > 0) log("info", `renderer workers: ${core._emulator_set_render_workers_ffi(renderWorkers)}`);
    return;
  }

  if (msg.type === "set-gpu-async") {
    gpuAsync = msg.on;
    if (core) applyGpuAsync(core);
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
      core?._emulator_pacing_resync_ffi(); /* paused time does not count */
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
