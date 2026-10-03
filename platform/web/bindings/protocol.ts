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
  /* The renderer: the GPU worker's WebGPU renderer (draws stream through the
   * gpu_ring region) or the software reference renderer. */
  | { readonly type: "set-gpu-mode"; readonly on: boolean }
  /* Host threads for guest threads (docs/PARALLEL.md; 0 = serial). */
  | { readonly type: "set-host-cores"; readonly cores: number }
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

export type CPUToMainMessage =
  | { readonly type: "layout"; readonly layout: MemoryLayout }
  | { readonly type: "ready"; readonly backendName: string; readonly backendVersion: string }
  | { readonly type: "log"; readonly level: LogLevel; readonly message: string }
  | { readonly type: "halted" }
  | { readonly type: "error"; readonly message: string }
  | { readonly type: "game-loaded"; readonly titleId: string; readonly entryPoint: bigint }
  | { readonly type: "load-failed"; readonly failure: LoadFailure }
  | { readonly type: "sd-files-added"; readonly added: readonly string[]; readonly failed: readonly string[] }
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
  | { readonly type: "resize"; readonly width: number; readonly height: number };

export type GPUToMainMessage =
  | { readonly type: "ready"; readonly adapterName: string | null; readonly streamRenderer: boolean }
  | { readonly type: "log"; readonly level: LogLevel; readonly message: string }
  | { readonly type: "error"; readonly message: string };
