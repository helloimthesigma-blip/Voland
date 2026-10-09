/**
 * Game-load outcome model shared by the CPU worker and the UI. The core
 * reports a numeric Result (core/common/result.h) plus a static message;
 * this module turns that into the small set of outcomes the user can act
 * on. Detection of encrypted input is the core's job (structural, §12
 * "Loader subsystem note") - nothing here inspects file bytes or names.
 *
 * Plain objects rather than `enum`: Node's --experimental-strip-types
 * (npm test) cannot run TypeScript enums.
 */

/** Mirrors `Result` in core/common/result.h. */
export const CoreResult = {
  Ok:              0,
  OutOfMemory:     1,
  InvalidArgument: 2,
  NotFound:        3,
  IoError:         4,
  NotImplemented:  5,
  MemoryFault:     6,
  NotContiguous:   7,
  EncryptedInput:  8,
} as const;

export type LoadFailureReason =
  /** Not pre-decrypted (or not an NCA at all) - same user action (§1.6). */
  | "encrypted-input"
  /** Decrypted NCA, but not one the loader can boot (no ExeFS, bad npdm, ...). */
  | "unsupported-content"
  /** Reading the user's file failed (permission revoked, file moved). */
  | "read-failed"
  /** The core is not available or failed internally. */
  | "internal";

export interface LoadFailure {
  readonly reason:  LoadFailureReason;
  /** The core's own message, shown verbatim as detail. */
  readonly message: string;
}

/** Mirrors Emulator_Status in core/emulator.h. */
export const EmulatorStatus = {
  Running:   0,
  Idle:      1,
  Exited:    2,
  Crashed:   3,
  Deadlock:  4,
  NotLoaded: 5,
} as const;

export type RunState = "running" | "exited" | "crashed" | "deadlock" | "paused";

/** Which run state a slice status ends in, or null while it keeps running. */
export function runStateAfterSlice(status: number): RunState | null {
  switch (status) {
    case EmulatorStatus.Running:
    case EmulatorStatus.Idle:
      return null;
    case EmulatorStatus.Exited:
      return "exited";
    case EmulatorStatus.Deadlock:
      return "deadlock";
    default:
      return "crashed";
  }
}

/** What the main thread hands the UI for one load request. */
export type GameLoadOutcome =
  | { readonly success: true;  readonly titleId: string; readonly entryPoint: bigint }
  | { readonly success: false; readonly failure: LoadFailure };

/** Maps a non-OK core Result to the user-actionable failure category. */
export function loadFailureFromResult(code: number, message: string): LoadFailure {
  switch (code) {
    case CoreResult.EncryptedInput:
      return { reason: "encrypted-input", message };
    case CoreResult.InvalidArgument:
    case CoreResult.NotFound:
    case CoreResult.NotImplemented:
      return { reason: "unsupported-content", message };
    case CoreResult.IoError:
      return { reason: "read-failed", message };
    default:
      return { reason: "internal", message };
  }
}

const TITLE_ID_HEX_DIGITS = 16;

/** A 64-bit program id as Horizon writes it: 16 upper-case hex digits. */
export function formatTitleId(programId: bigint): string {
  return BigInt.asUintN(64, programId).toString(16).toUpperCase().padStart(TITLE_ID_HEX_DIGITS, "0");
}

/**
 * Reads a NUL-terminated UTF-8 string out of linear memory. Used for the
 * core's static Error messages; `maxBytes` bounds the scan so a bad
 * pointer can never walk the whole 5.25GiB memory.
 */
export function readCString(buffer: ArrayBufferLike, address: number, maxBytes: number): string {
  if (address === 0) return "";
  const window = new Uint8Array(buffer, address, Math.min(maxBytes, buffer.byteLength - address));
  const end = window.indexOf(0);
  const bytes = window.slice(0, end === -1 ? window.length : end);
  return new TextDecoder().decode(bytes);
}

/** System archives a title may mount (fsp-srv OpenDataStorageByDataId)
 * that Voland does not ship (§1.6): the user adds their own console's
 * copy under "System files". `id` is the data id in 16 hex digits. */
export interface SystemFileInfo {
  readonly id: string;
  readonly name: string;
  readonly neededBy: string;
}
export const SYSTEM_FILES: readonly SystemFileInfo[] = [
  { id: "0100000000000802", name: "Mii model", neededBy: "games that draw Miis (Mario Kart 8 Deluxe, Mii Fighters in Smash)" },
];
export function systemFileName(id: string): string {
  return SYSTEM_FILES.find((f) => f.id === id)?.name ?? `system data ${id}`;
}

/** What adding system files did: the archives stored (data ids), how
 * many files were not ones games need (the rest of a firmware folder),
 * and why any file that looked usable could not be used. */
export interface SystemFileOutcome {
  readonly added: readonly string[];
  readonly skipped: number;
  readonly errors: readonly string[];
}

/** The shell's handle on "System files" (main.ts -> LoadPanel). */
export interface SystemFilesApi {
  readonly add: (files: readonly File[], id: string | null) => Promise<SystemFileOutcome>;
  readonly list: () => Promise<readonly string[]>;
}

/** What an SD-card import did: the SD paths written and the files that
 * could not be (too large, out of space, unreadable). */
export interface SdImportOutcome {
  readonly added: readonly string[];
  readonly failed: readonly string[];
}
