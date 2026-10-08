/**
 * Typed message protocol between the main thread and the workers. See
 * docs/DESIGN.md section 16 ("Worker message protocol - lifecycle only").
 * Per-frame data (input, frames, audio, GPU commands) never travels over
 * postMessage - it lives in the single shared WebAssembly.Memory (§4) and
 * is addressed through MemoryLayout offsets. Phase 0 ships the minimum
 * lifecycle surface: init + log plumbing + halt. Phase 1 adds the game
 * load request/outcome pair; gamepad connection and save-state messages
 * arrive in later phases.
 *
 * `load-game` carries a `File`, not §16's `titleId + FileSystemFileHandle`:
 * `File` is what both the FSA path (`handle.getFile()`) and the
 * non-Chromium `<input type="file">` fallback (§15) produce, and the
 * title id is an output of parsing (main.npdm), not an input the main
 * thread could know. The worker reads it piecewise; it is never copied
 * into linear memory whole.
 */
import type { LoadFailure, RunState } from "./load";
import type { MemoryLayout } from "./layout";

export type LogLevel = "trace" | "debug" | "info" | "warn" | "error";

export type MainToCPUMessage =
  | { readonly type: "init"; readonly memory: WebAssembly.Memory }
  | { readonly type: "load-game"; readonly file: File }
  /* Copy files onto the emulated SD card (§15): .nro files go to /switch/
   * (where homebrew menus look), anything else to the root. Answered by
   * exactly one sd-files-added. */
  | { readonly type: "sd-add-files"; readonly files: readonly File[] }
  /* Empty the SD card (and its stored copy). Answered by sd-files-added
   * with nothing added. */
  | { readonly type: "sd-clear" }
  /* Game saves (§15, workers/save-store.ts): a tar of every stored save,
   * answered by saves-exported; or a tar to store (and load into the
   * core), answered by saves-imported. */
  | { readonly type: "export-saves" }
  | { readonly type: "import-saves"; readonly tar: ArrayBuffer }
  /* The save browser (src/ui/SavesPanel.tsx): every stored save archive
   * (saves-listed); one archive to replace a save with, loaded into the
   * core and stored (save-put); a save to delete (save-deleted). */
  | { readonly type: "list-saves" }
  | { readonly type: "put-save"; readonly name: string; readonly archive: ArrayBuffer }
  | { readonly type: "delete-save"; readonly name: string }
  /* Save states (workers/savestate.ts): freeze the running game into a new
   * state (state-saved), resume one (state-loaded), list (states-listed)
   * or delete (state-deleted) them. */
  /* The compatibility report's core half (answered by compat-report). */
  | { readonly type: "compat-report" }
  | { readonly type: "save-state" }
  | { readonly type: "load-state"; readonly id: string }
  | { readonly type: "list-states" }
  | { readonly type: "delete-state"; readonly id: string }
  /* §18: slot connect/disconnect is a lifecycle event; the state itself
   * travels through the input region, never postMessage. */
  | { readonly type: "controller-connected"; readonly index: number; readonly profileId: number }
  | { readonly type: "controller-disconnected"; readonly index: number }
  | { readonly type: "pause" }
  | { readonly type: "resume" }
  /* The player's answer to a text-input-request (software keyboard). */
  | { readonly type: "text-input-response"; readonly text: string; readonly accepted: boolean }
  /* Frame skip, a user setting: rasterise one of every frames + 1 frames. */
  | { readonly type: "set-frame-skip"; readonly frames: number }
  | { readonly type: "set-gpu-async"; readonly on: boolean }
  | { readonly type: "set-render-workers"; readonly count: number }
  | { readonly type: "dump-threads"; readonly backtrace: boolean }
  | { readonly type: "set-gpu-compute"; readonly on: boolean }
  /* The renderer: the GPU worker's WebGPU renderer (draws stream through the
   * gpu_ring region) or the software reference renderer. */
  | { readonly type: "set-gpu-mode"; readonly on: boolean }
  /* Host threads for guest threads (docs/PARALLEL.md; 0 = serial). */
  | { readonly type: "set-host-cores"; readonly cores: number }
  /* Poll coalescing (docs/PARALLEL.md "Polling threads"); on by default. */
  | { readonly type: "set-poll-coalescing"; readonly on: boolean }
  /* Free-running guest cores (docs/PARALLEL.md; prototype, off by default). */
  | { readonly type: "set-free-running"; readonly fromSlice: number } /* 0 = off */
  /* Wall-clock pacing (virtual time keeps up with wall time); on by default. */
  | { readonly type: "set-pacing"; readonly on: boolean }
  | { readonly type: "halt" };

/** A software-keyboard prompt (§12 library applets): a title waits for
 * text. Rare and user-paced - a lifecycle event, not per-frame data. */
export interface TextInputRequest {
  readonly header: string;
  readonly sub: string;
  readonly guide: string;
  readonly initial: string;
  readonly maxLength: number;
  readonly minLength: number;
  readonly password: boolean;
}

/** What the core knows about how a title is running (compatibility report). */
export interface CoreReport {
  readonly titleId: string | null;
  readonly backend: string;
  readonly slices: number;
  readonly virtualSeconds: number;
  readonly wallSeconds: number;
  /** "count<TAB>line" per distinct warning/error, first seen first. */
  readonly problems: string;
  /** emulator_render_stats_ffi's counters, by name. */
  readonly render: Readonly<Record<string, number>>;
}

/** One stored save state, as the UI lists it. */
export interface SavestateInfo {
  readonly id: string;
  readonly titleId: string;
  readonly createdAt: number;
  /** The game's virtual time when it was taken. */
  readonly virtualSeconds: number;
  readonly bytes: number;
}

/** One stored save: its name ("SS-<attribute hex>") and archive bytes. */
export interface StoredSave {
  readonly name: string;
  readonly archive: ArrayBuffer;
}

export type CPUToMainMessage =
  | { readonly type: "layout"; readonly layout: MemoryLayout }
  | { readonly type: "ready"; readonly backendName: string; readonly backendVersion: string }
  | { readonly type: "log"; readonly level: LogLevel; readonly message: string }
  | { readonly type: "halted" }
  | { readonly type: "error"; readonly message: string }
  | { readonly type: "game-loaded"; readonly titleId: string; readonly entryPoint: bigint }
  | { readonly type: "load-failed"; readonly failure: LoadFailure }
  | { readonly type: "sd-files-added"; readonly added: readonly string[]; readonly failed: readonly string[] }
  | { readonly type: "saves-exported"; readonly tar: ArrayBuffer; readonly count: number }
  | { readonly type: "saves-imported"; readonly imported: number; readonly rejected: number }
  | { readonly type: "saves-listed"; readonly saves: readonly StoredSave[] }
  | { readonly type: "save-put"; readonly name: string; readonly ok: boolean }
  | { readonly type: "save-deleted"; readonly name: string; readonly ok: boolean }
  | { readonly type: "compat-report"; readonly report: CoreReport }
  | { readonly type: "state-saved"; readonly info: SavestateInfo | null; readonly error: string }
  | { readonly type: "state-loaded"; readonly ok: boolean; readonly error: string }
  | { readonly type: "states-listed"; readonly states: readonly SavestateInfo[] }
  | { readonly type: "state-deleted"; readonly id: string }
  /* Guest debug text (svcOutputDebugString) and run-state changes: both
   * happen at guest pace, never per frame (§6). */
  | { readonly type: "guest-output"; readonly text: string }
  | { readonly type: "run-state"; readonly state: RunState; readonly detail: string }
  | { readonly type: "text-input-request"; readonly request: TextInputRequest };

export type MainToGPUMessage =
  | {
      readonly type: "init";
      readonly canvas: OffscreenCanvas;
      readonly memory: WebAssembly.Memory;
      readonly layout: MemoryLayout;
    }
  | { readonly type: "resize"; readonly width: number; readonly height: number }
  /* The title that just loaded (lifecycle): its persistent shader cache
   * is opened and pre-built (workers/shader-cache.ts). */
  | { readonly type: "title"; readonly titleId: string };

export type GPUToMainMessage =
  | { readonly type: "ready"; readonly adapterName: string | null; readonly streamRenderer: boolean }
  | { readonly type: "log"; readonly level: LogLevel; readonly message: string }
  | { readonly type: "error"; readonly message: string };
