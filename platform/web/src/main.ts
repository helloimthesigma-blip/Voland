/**
 * Boot sequence (docs/DESIGN.md section 16). The page loads as a
 * black screen with a boot log on the right; once the CPU and GPU workers
 * each report "ready" we dynamically import the Solid shell and fade the
 * boot overlay out. Keeping the Solid runtime out of the boot-critical
 * path means the black screen appears immediately and the UI chunk only
 * loads once the core has actually come up.
 *
 * There is no Audio Worker (§14) - the audio ring is drained directly by
 * an AudioWorkletProcessor once DSP HLE exists (Phase 4). Phase 0 only
 * needs the CPU and GPU workers to validate the boot path end to end.
 */

import type { CPUToMainMessage, GPUToMainMessage, MainToCPUMessage, MainToGPUMessage } from "@bindings/protocol";
import { AUDIO_RING_CAPACITY_FRAMES, type MemoryLayout, toByteOffset } from "@bindings/layout";
import type { GameLoadOutcome, SdImportOutcome } from "@bindings/load";
import type { MainToVideoMessage, VideoToMainMessage } from "@bindings/video";
import { handleSavesMessage, registerSavesWorker, rememberTitleName } from "./saves";
import { handleSavestateMessage, registerSavestateWorker } from "./savestates";
import { handleReportMessage, registerReportWorker } from "./compat-report";
import { detectCapabilities, type PlatformCapabilities } from "./capabilities";
import { publishBootMilestone } from "./e2e-hooks";
import { PUBLISH_INDEX } from "@bindings/framebuffer";
import { GPU_STREAM_HEADER_BYTES, OFF_PRESENTS } from "@bindings/gpu-records";
import { appendGuestOutput, resetGuestConsole, setGuestFps, setGuestRunState } from "./guest-console";
import { clearTextInput, showTextInput } from "./text-input-store";
import { startAudioOutput } from "./audio/audio-output";
import { startInputLoop } from "./input/input-loop";
import { getBindings } from "./input/bindings-store";
import { appendLogLine, setStatus } from "./log";

/**
 * Total shared linear memory: guest RAM + every layout.h region +
 * Emscripten's own data/stack/heap (~5.25GiB). Fixed at boot -
 * `initial === maximum`, growth disabled, so views never detach (§4).
 * This MUST equal CMakeLists.txt's INITIAL_MEMORY in bytes (with growth
 * disabled it is also the maximum; there is no separate MAXIMUM_MEMORY)
 * (5_637_144_576) or the core module fails to instantiate.
 */
const WASM_PAGE_BYTES = 65_536n;
const TOTAL_MEMORY_BYTES = 5_637_144_576n; // ~5.25GiB
const TOTAL_MEMORY_PAGES = TOTAL_MEMORY_BYTES / WASM_PAGE_BYTES; // 86016n

const WORKER_READY_TIMEOUT_MS = 10_000;

function fatal(reason: string): void {
  setStatus(`fatal: ${reason}`);
  appendLogLine("error", reason);
}

function reportCapabilities(caps: PlatformCapabilities): void {
  const lines: string[] = [];
  (Object.keys(caps) as Array<keyof PlatformCapabilities>).forEach(key => {
    lines.push(`${key}=${caps[key] ? "yes" : "no"}`);
  });
  appendLogLine("debug", `capabilities: ${lines.join(", ")}`);
}

/** Guarded one-time reload so a fresh visit under SW-injected COOP/COEP
 * headers (rather than real server headers) gets a second pass once the
 * Service Worker actually controls the page (§16). */
async function reloadOnceForCrossOriginIsolation(): Promise<boolean> {
  if (sessionStorage.getItem("voland-coi-reload-attempted")) return false;
  sessionStorage.setItem("voland-coi-reload-attempted", "1");
  if ("serviceWorker" in navigator) {
    await navigator.serviceWorker.ready;
  }
  location.reload();
  return true;
}

/**
 * lib.dom.d.ts's `WebAssembly.MemoryDescriptor` doesn't know about the
 * memory64 shape yet: `initial`/`maximum` are BigInt page counts, not
 * Number, once `address: "i64"` is present.
 */
interface Memory64Descriptor {
  readonly initial: bigint;
  readonly maximum: bigint;
  readonly shared: true;
  readonly address: "i64";
}

function allocateSharedMemory(): WebAssembly.Memory | null {
  // Verified directly against this environment's V8 and against
  // Emscripten's own -m64 (wasm64) glue output: both use `address: "i64"`
  // with BigInt page counts, NOT `index: "i64"` with Number page counts.
  // The latter is silently accepted as an unrecognized property and falls
  // back to an ordinary 32-bit memory, which then throws RangeError the
  // moment page counts exceed the wasm32 cap of 65536 pages (4GiB) -
  // exactly this case. (docs/DESIGN.md's own §16 example uses `index`;
  // that's stale relative to what actually shipped - do not copy it.)
  const descriptor: Memory64Descriptor = {
    initial: TOTAL_MEMORY_PAGES,
    maximum: TOTAL_MEMORY_PAGES,
    shared: true,
    address: "i64",
  };
  try {
    return new WebAssembly.Memory(descriptor as unknown as WebAssembly.MemoryDescriptor);
  } catch (e) {
    appendLogLine("error", `WebAssembly.Memory allocation failed: ${(e as Error).message}`);
    return null;
  }
}

function attachWorkerLogRelay<TMessage extends { type: string }>(
  workerName: string,
  worker: Worker,
  readyHandler: (msg: TMessage) => void,
  failHandler: (reason: string) => void,
): void {
  worker.addEventListener("message", (event: MessageEvent<TMessage>) => {
    const msg = event.data;
    if (msg.type === "log") {
      const logMsg = msg as TMessage & { level: "debug" | "info" | "warn" | "error"; message: string };
      appendLogLine(logMsg.level, `${workerName}: ${logMsg.message}`);
      return;
    }
    if (msg.type === "error") {
      const errMsg = msg as TMessage & { message: string };
      appendLogLine("error", `${workerName} error: ${errMsg.message}`);
      failHandler(errMsg.message);
      return;
    }
    readyHandler(msg);
  });
  worker.addEventListener("error", (event: ErrorEvent) => {
    appendLogLine("error", `${workerName} uncaught: ${event.message}`);
    failHandler(event.message || "uncaught worker error");
  });
}

/* Video decode (§13): a WebCodecs worker reading the core's NVDEC
 * requests from the video region; lifecycle-only messages. */
function startVideoWorker(memory: WebAssembly.Memory, layout: MemoryLayout): void {
  const videoWorker = new Worker(new URL("../workers/video.worker.ts", import.meta.url), { type: "module" });
  attachWorkerLogRelay<VideoToMainMessage>("video", videoWorker, msg => {
    if (msg.type === "ready") appendLogLine("info", `video: WebCodecs ${msg.webCodecs ? "available" : "unavailable"}`);
  }, () => undefined);
  videoWorker.postMessage({ type: "init", memory, layout } satisfies MainToVideoMessage);
}

interface BootResult {
  readonly adapterLabel: string;
  readonly cpuBackend:   string;
  readonly guestRamMiB:  number;
  readonly loadGame:     (file: File) => Promise<GameLoadOutcome>;
  readonly addToSdCard:  (files: readonly File[]) => Promise<SdImportOutcome>;
  readonly clearSdCard:  () => Promise<SdImportOutcome>;
  readonly setFrameSkip: (frames: number) => void;
  readonly setHostCores: (cores: number) => void;
  readonly setPaused:    (paused: boolean) => void;
  readonly respondText:  (text: string, accepted: boolean) => void;
}

async function boot(): Promise<BootResult | null> {
  appendLogLine("info", "Voland web booting");

  if ("serviceWorker" in navigator) {
    // Vite serves sw.ts directly (on-the-fly transform) in dev; the
    // production build emits it as a stable, unhashed sw.js (vite.config.ts).
    const swUrl = `${import.meta.env.BASE_URL}${import.meta.env.DEV ? "sw.ts" : "sw.js"}`;
    try {
      await navigator.serviceWorker.register(swUrl, { type: "module" });
    } catch (e) {
      appendLogLine("warn", `Service Worker registration failed: ${(e as Error).message}`);
    }
  }

  if (!crossOriginIsolated) {
    const reloaded = await reloadOnceForCrossOriginIsolation();
    if (reloaded) return null; // navigation is in flight
    fatal("Cross-origin isolation is not active. Ensure the server sets " +
          "COOP: same-origin and COEP: require-corp (§16).");
    return null;
  }

  const caps = detectCapabilities();
  reportCapabilities(caps);

  if (!caps.webGPU) {
    fatal("WebGPU is not available. Update your browser (WebGPU is Baseline " +
          "since Jan 2026: Chrome/Edge 113+, Safari 26+, Firefox 141+).");
    return null;
  }
  if (!caps.opfs) {
    fatal("Origin Private Filesystem (OPFS) is not available.");
    return null;
  }

  /* ONE memory. Guest RAM and every other shared region live inside it
   * (§4). No separate SharedArrayBuffers. */
  const memory = allocateSharedMemory();
  if (!memory) {
    fatal("Could not allocate emulator memory (~5.25GiB). Your browser or " +
          "device limits shared memory; use a native build.");
    return null;
  }
  appendLogLine("info", `allocated shared memory: ${TOTAL_MEMORY_PAGES} pages (~5.25GiB)`);
  publishBootMilestone(memory);

  const canvas = document.getElementById("game") as HTMLCanvasElement | null;
  if (!canvas) {
    fatal("boot: #game canvas missing from index.html");
    return null;
  }
  const offscreen = canvas.transferControlToOffscreen();

  const cpuWorker = new Worker(new URL("../workers/cpu.worker.ts", import.meta.url), { type: "module" });
  registerSavesWorker(cpuWorker);
  registerSavestateWorker(cpuWorker);
  registerReportWorker(cpuWorker);
  const gpuWorker = new Worker(new URL("../workers/gpu.worker.ts", import.meta.url), { type: "module" });

  type Slot = "pending" | "ready" | "failed";
  let cpuSlot: Slot = "pending";
  let gpuSlot: Slot = "pending";

  /* At most one load in flight: the worker answers each load-game with
   * exactly one game-loaded or load-failed, in order. */
  let pendingLoad: ((outcome: GameLoadOutcome) => void) | null = null;
  let loadingFileName = "";
  /* SD imports queue in order; the worker answers each with one sd-files-added. */
  const pendingSd: ((outcome: SdImportOutcome) => void)[] = [];

  let cpuBackend:   string | null = null;
  let adapterLabel: string | null = null;
  let layout:       MemoryLayout | null = null;

  function slotGlyph(slot: Slot): string {
    return slot === "ready" ? "ok" : slot === "failed" ? "fail" : "…";
  }
  function updateStatus(): void {
    setStatus([`cpu=${slotGlyph(cpuSlot)}`, `gpu=${slotGlyph(gpuSlot)}`].join(" · "));
  }
  updateStatus();

  const cpuReady = new Promise<void>((resolve) => {
    attachWorkerLogRelay<CPUToMainMessage>("cpu", cpuWorker, msg => {
      if (msg.type === "layout") {
        layout = msg.layout;
        appendLogLine("info", `layout handshake: guestRamBase=0x${layout.guestRamBase.toString(16)}`);
        gpuWorker.postMessage(
          { type: "init", canvas: offscreen, memory, layout } satisfies MainToGPUMessage,
          [offscreen],
        );
        startVideoWorker(memory, layout);
        return;
      }
      if (msg.type === "ready") {
        cpuBackend = `${msg.backendName} v${msg.backendVersion}`;
        appendLogLine("info", `cpu backend: ${cpuBackend}`);
        cpuSlot = "ready";
        updateStatus();
        resolve();
      } else if (msg.type === "halted") {
        appendLogLine("warn", "cpu halted");
      } else if (msg.type === "game-loaded") {
        appendLogLine("info", `loaded title ${msg.titleId}, entry 0x${msg.entryPoint.toString(16)}`);
        if (loadingFileName) rememberTitleName(msg.titleId, loadingFileName);
        gpuWorker.postMessage({ type: "title", titleId: msg.titleId } satisfies MainToGPUMessage);
        pendingLoad?.({ success: true, titleId: msg.titleId, entryPoint: msg.entryPoint });
        pendingLoad = null;
      } else if (msg.type === "guest-output") {
        appendGuestOutput(msg.text);
      } else if (msg.type === "text-input-request") {
        showTextInput(msg.request);
      } else if (msg.type === "run-state") {
        appendLogLine(msg.state === "crashed" ? "error" : "info", `guest ${msg.state}${msg.detail ? `: ${msg.detail}` : ""}`);
        setGuestRunState(msg.state, msg.detail);
      } else if (handleSavesMessage(msg)) {
        /* game-save backup replies (src/saves.ts) */
      } else if (handleReportMessage(msg)) {
        /* compatibility report (src/compat-report.ts) */
      } else if (handleSavestateMessage(msg)) {
        /* save-state replies (src/savestates.ts) */
      } else if (msg.type === "sd-files-added") {
        appendLogLine(msg.failed.length ? "warn" : "info",
          `SD card: added ${msg.added.length} file(s)${msg.failed.length ? `, failed: ${msg.failed.join(", ")}` : ""}`);
        pendingSd.shift()?.({ added: msg.added, failed: msg.failed });
      } else if (msg.type === "load-failed") {
        appendLogLine("warn", `load failed (${msg.failure.reason}): ${msg.failure.message}`);
        pendingLoad?.({ success: false, failure: msg.failure });
        pendingLoad = null;
      }
    }, () => { cpuSlot = "failed"; updateStatus(); resolve(); });
  });

  const gpuReady = new Promise<void>((resolve) => {
    attachWorkerLogRelay<GPUToMainMessage>("gpu", gpuWorker, msg => {
      if (msg.type === "ready") {
        adapterLabel = msg.adapterName ?? "Software/Unknown";
        appendLogLine("info", `gpu adapter: ${adapterLabel}`);
        /* WebGPU renderer unless ?renderer=software asks for the reference. */
        const software = new URLSearchParams(location.search).get("renderer") === "software";
        if (msg.streamRenderer && !software) {
          cpuWorker.postMessage({ type: "set-gpu-mode", on: true } satisfies MainToCPUMessage);
        }
        /* ?cores=N: host threads for guest threads (0 = serial). */
        const cores = new URLSearchParams(location.search).get("cores");
        if (cores !== null && /^[0-9]+$/.test(cores)) {
          cpuWorker.postMessage({ type: "set-host-cores", cores: Number(cores) } satisfies MainToCPUMessage);
        }
        /* ?polls=0: the plain scheduler (no poll coalescing), for A/B runs. */
        if (new URLSearchParams(location.search).get("polls") === "0") {
          cpuWorker.postMessage({ type: "set-poll-coalescing", on: false } satisfies MainToCPUMessage);
        }
        /* ?pacing=0: no wall-clock pacing (the game runs in slow motion when
         * the host is slower than the Switch). */
        /* ?rworkers=N: the renderer's threads (tuning). */
        const rworkers = new URLSearchParams(location.search).get("rworkers");
        if (rworkers !== null && /^[0-9]+$/.test(rworkers)) {
          cpuWorker.postMessage({ type: "set-render-workers", count: Number(rworkers) } satisfies MainToCPUMessage);
        }
        /* ?gpuasync=1: GPU command processing on its own thread. */
        const gpuAsyncParam = new URLSearchParams(location.search).get("gpuasync");
        if (gpuAsyncParam === "0" || gpuAsyncParam === "1") {
          cpuWorker.postMessage({ type: "set-gpu-async", on: gpuAsyncParam === "1" } satisfies MainToCPUMessage);
        }
        if (new URLSearchParams(location.search).get("pacing") === "0") {
          cpuWorker.postMessage({ type: "set-pacing", on: false } satisfies MainToCPUMessage);
        }
        /* ?free=N: free-running guest cores (prototype) from scheduler
         * slice N on (1 = from the start); slice-counted warm-ups and input
         * recipes then still mean what they did. */
        const free = new URLSearchParams(location.search).get("free");
        if (free !== null && /^[0-9]+$/.test(free) && Number(free) > 0) {
          cpuWorker.postMessage({ type: "set-free-running", fromSlice: Number(free) } satisfies MainToCPUMessage);
        }
        gpuSlot = "ready";
        updateStatus();
        resolve();
      }
    }, () => { gpuSlot = "failed"; updateStatus(); resolve(); });
  });

  const timeout = new Promise<void>((resolve) => {
    window.setTimeout(() => {
      if (cpuSlot === "pending") { cpuSlot = "failed"; appendLogLine("error", `cpu worker did not report within ${WORKER_READY_TIMEOUT_MS}ms`); }
      if (gpuSlot === "pending") { gpuSlot = "failed"; appendLogLine("error", `gpu worker did not report within ${WORKER_READY_TIMEOUT_MS}ms`); }
      updateStatus();
      resolve();
    }, WORKER_READY_TIMEOUT_MS);
  });

  cpuWorker.postMessage({ type: "init", memory } satisfies MainToCPUMessage);
  appendLogLine("info", "cpu worker posted init message; awaiting layout handshake");

  await Promise.race([Promise.all([cpuReady, gpuReady]), timeout]);

  /* Input (§18): written into the shared region every frame once the
   * layout is known; only connect/disconnect goes to the worker (§16). */
  const inputLayout = layout as MemoryLayout | null;
  if (inputLayout) {
    startInputLoop({
      buffer: memory.buffer,
      regionBase: toByteOffset(inputLayout.inputRegionBase),
      getGamepads: () => navigator.getGamepads(),
      touchTarget: canvas,
      guestFps: () => window.__VOLAND_STATS__?.fps ?? 0,
      bindings: getBindings,
      onConnectionChange: (change) => {
        const msg: MainToCPUMessage = change.connected
          ? { type: "controller-connected", index: change.slot, profileId: change.profileId }
          : { type: "controller-disconnected", index: change.slot };
        cpuWorker.postMessage(msg);
      },
    });
  }

  /* rAF stops in hidden tabs, freezing input writes (§18); auto-pause
   * keeps the policy explicit instead of leaving it as a silent symptom. */
  document.addEventListener("visibilitychange", () => {
    const msg: MainToCPUMessage = { type: document.hidden ? "pause" : "resume" };
    cpuWorker.postMessage(msg);
  });

  // TypeScript's control flow analysis doesn't see assignments made inside
  // closures (the "layout" case above), so it treats `layout` as still
  // `null` here; the cast reasserts the declared type.
  const finalLayout = layout as MemoryLayout | null;

  /* Frame-rate meter: the framebuffer slots' publish counter, sampled once
   * a second straight from shared memory (no per-frame messages, §6). */
  let lastPublished = -1;
  setInterval(() => {
    const current = layout as MemoryLayout | null;
    if (!current) return;
    const counters = new Int32Array(memory.buffer, toByteOffset(current.framebufferSlotBase), 2);
    /* Frames the WebGPU renderer presented count too (the GPU stream's counter). */
    const stream = new Int32Array(memory.buffer, toByteOffset(current.gpuRingBase), GPU_STREAM_HEADER_BYTES / 4);
    const published = (Atomics.load(counters, PUBLISH_INDEX) + Atomics.load(stream, OFF_PRESENTS / 4)) | 0;
    const fps = lastPublished < 0 ? 0 : (published - lastPublished) | 0;
    lastPublished = published;
    setGuestFps(fps);
    window.__VOLAND_STATS__ = { fps, presents: published };
  }, 1000);

  function loadGame(file: File): Promise<GameLoadOutcome> {
    if (pendingLoad) {
      return Promise.resolve<GameLoadOutcome>({
        success: false,
        failure: { reason: "internal", message: "another game is still loading" },
      });
    }
    if (cpuSlot !== "ready") {
      return Promise.resolve<GameLoadOutcome>({
        success: false,
        failure: { reason: "internal", message: "the CPU worker is not running, so nothing can be loaded" },
      });
    }
    /* A load is a user gesture: the moment browsers allow audio to start. */
    if (finalLayout && memory) {
      void startAudioOutput(memory, toByteOffset(finalLayout.audioRingBase), AUDIO_RING_CAPACITY_FRAMES,
                            (message) => appendLogLine("info", message));
    }
    return new Promise<GameLoadOutcome>((resolve) => {
      pendingLoad = resolve;
      loadingFileName = file.name;
      resetGuestConsole();
      cpuWorker.postMessage({ type: "load-game", file } satisfies MainToCPUMessage);
    });
  }

  function addToSdCard(files: readonly File[]): Promise<SdImportOutcome> {
    if (cpuSlot !== "ready") return Promise.resolve<SdImportOutcome>({ added: [], failed: files.map((f) => f.name) });
    return new Promise<SdImportOutcome>((resolve) => {
      pendingSd.push(resolve);
      cpuWorker.postMessage({ type: "sd-add-files", files } satisfies MainToCPUMessage);
    });
  }

  function respondText(text: string, accepted: boolean): void {
    clearTextInput();
    cpuWorker.postMessage({ type: "text-input-response", text, accepted } satisfies MainToCPUMessage);
  }

  function setFrameSkip(frames: number): void {
    cpuWorker.postMessage({ type: "set-frame-skip", frames } satisfies MainToCPUMessage);
  }

  /** -1: the default host cores; 0: the serial scheduler (ThreadsSetting). */
  function setHostCores(cores: number): void {
    cpuWorker.postMessage({ type: "set-host-cores", cores } satisfies MainToCPUMessage);
  }

  function setPaused(paused: boolean): void {
    cpuWorker.postMessage((paused ? { type: "pause" } : { type: "resume" }) satisfies MainToCPUMessage);
  }

  function clearSdCard(): Promise<SdImportOutcome> {
    if (cpuSlot !== "ready") return Promise.resolve<SdImportOutcome>({ added: [], failed: [] });
    return new Promise<SdImportOutcome>((resolve) => {
      pendingSd.push(resolve);
      cpuWorker.postMessage({ type: "sd-clear" } satisfies MainToCPUMessage);
    });
  }

  return {
    adapterLabel: adapterLabel ?? "unavailable",
    cpuBackend:   cpuBackend   ?? "unavailable",
    guestRamMiB:  finalLayout ? Number(finalLayout.guestRamSize / (1024n * 1024n)) : 0,
    loadGame,
    addToSdCard,
    clearSdCard,
    setPaused,
    setFrameSkip,
    setHostCores,
    respondText,
  };
}

boot()
  .then(async result => {
    if (!result) return;
    appendLogLine("info", "core online - mounting Solid shell");
    const { mountShell } = await import("./ui/mount");
    mountShell(result);
  })
  .catch(e => fatal(`unhandled boot error: ${(e as Error).message}`));
