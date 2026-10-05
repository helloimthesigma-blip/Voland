/**
 * Save states in the browser (DESIGN.md §15 "save states"; core API in
 * core/emulator.h emulator_savestate_*). The core names the linear-memory
 * ranges that hold the machine; this copies them into one OPFS file per
 * state, skipping all-zero chunks (most of guest RAM), and back.
 *
 *   "savestates/<id>.vstate":
 *     u32 magic "VSST", u32 format version, u32 plan bytes, u32 chunk bytes,
 *     the core's Savestate_Plan as it was, then per range a u32 bitmap
 *     length and the bitmap (bit c: chunk c is stored), then the stored
 *     chunks in range/chunk order (a range's last chunk may be short).
 *   "savestates/<id>.json": what the UI lists (title, time, size).
 *
 * The plan's addresses identify the session layout: a state only loads
 * where every range sits at the same address with the same capacity (the
 * same build, the same game, a fresh page load reaching the same layout).
 * Reads and writes use synchronous access handles while the machine is
 * frozen between slices, so what is stored is one consistent instant.
 */
import type { SavestateInfo } from "@bindings/protocol";

export const SAVESTATE_DIRECTORY = "savestates";
const MAGIC = 0x54535356; /* "VSST" */
const FORMAT_VERSION = 1;
export const CHUNK_BYTES = 64 * 1024;
const HEADER_BYTES = 16;

/* core/emulator.h Savestate_Plan (wasm64, little-endian). */
const PLAN_VERSION = 0;
const PLAN_RANGE_COUNT = 4;
const PLAN_PROGRAM_ID = 8;
const PLAN_EMULATOR_BYTES = 16;
const PLAN_VIRTUAL_TICKS = 24;
const PLAN_RANGES = 32;
const RANGE_BYTES = 32;
const RANGE_ADDRESS = 0;
const RANGE_LENGTH = 8;
const RANGE_CAPACITY = 16;
const RANGE_KIND = 24;
export const PLAN_MAX_RANGES = 12;
export const PLAN_BYTES = PLAN_RANGES + PLAN_MAX_RANGES * RANGE_BYTES;
const TIMER_HZ = 19_200_000;

export interface PlanRange {
  readonly address: number;
  readonly bytes: number;
  readonly capacity: number;
  readonly kind: number;
}

export interface Plan {
  readonly version: number;
  readonly programId: bigint;
  readonly emulatorBytes: number;
  readonly virtualTicks: number;
  readonly ranges: readonly PlanRange[];
  /** The plan's bytes as the core wrote them (stored verbatim). */
  readonly raw: Uint8Array;
}

/** Decodes a Savestate_Plan from `bytes` (PLAN_BYTES long). Pure. */
export function parsePlan(bytes: Uint8Array): Plan {
  const v = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const count = Math.min(v.getUint32(PLAN_RANGE_COUNT, true), PLAN_MAX_RANGES);
  const ranges: PlanRange[] = [];
  for (let i = 0; i < count; i++) {
    const at = PLAN_RANGES + i * RANGE_BYTES;
    ranges.push({
      address: Number(v.getBigUint64(at + RANGE_ADDRESS, true)),
      bytes: Number(v.getBigUint64(at + RANGE_LENGTH, true)),
      capacity: Number(v.getBigUint64(at + RANGE_CAPACITY, true)),
      kind: v.getUint32(at + RANGE_KIND, true),
    });
  }
  return {
    version: v.getUint32(PLAN_VERSION, true),
    programId: v.getBigUint64(PLAN_PROGRAM_ID, true),
    emulatorBytes: Number(v.getBigUint64(PLAN_EMULATOR_BYTES, true)),
    virtualTicks: Number(v.getBigUint64(PLAN_VIRTUAL_TICKS, true)),
    ranges,
    raw: bytes.slice(),
  };
}

/** Why a stored plan cannot be restored into the current one, or null. Pure. */
export function planMismatch(stored: Plan, now: Plan): string | null {
  if (stored.version !== now.version || stored.emulatorBytes !== now.emulatorBytes) {
    return "it was made by a different version of Voland";
  }
  if (stored.programId !== now.programId) return "it belongs to a different game";
  if (stored.ranges.length !== now.ranges.length) return "the emulator's memory is laid out differently now";
  for (let i = 0; i < stored.ranges.length; i++) {
    const a = stored.ranges[i] as PlanRange, b = now.ranges[i] as PlanRange;
    if (a.kind !== b.kind || a.address !== b.address || a.capacity !== b.capacity || a.bytes > b.capacity) {
      return "the emulator's memory is laid out differently in this session (reload the page and start the game, then load the state before doing anything else)";
    }
  }
  return null;
}

function chunkCount(bytes: number): number {
  return Math.ceil(bytes / CHUNK_BYTES);
}

function isZero(view: Uint8Array): boolean {
  const words = view.byteLength >>> 2;
  const w = new Uint32Array(view.buffer, view.byteOffset, words);
  for (let i = 0; i < words; i++) if (w[i] !== 0) return false;
  for (let i = words << 2; i < view.byteLength; i++) if (view[i] !== 0) return false;
  return true;
}

async function stateDirectory(): Promise<FileSystemDirectoryHandle> {
  const root = await navigator.storage.getDirectory();
  return root.getDirectoryHandle(SAVESTATE_DIRECTORY, { create: true });
}

async function syncHandle(name: string, create: boolean): Promise<FileSystemSyncAccessHandle> {
  const dir = await stateDirectory();
  const file = await dir.getFileHandle(name, { create });
  return (file as FileSystemFileHandle & { createSyncAccessHandle(): Promise<FileSystemSyncAccessHandle> }).createSyncAccessHandle();
}

/** Opens a new state file for writing (before the machine is frozen). */
export function createStateFile(id: string): Promise<FileSystemSyncAccessHandle> {
  return syncHandle(`${id}.vstate`, true);
}

export function openStateFile(id: string): Promise<FileSystemSyncAccessHandle> {
  return syncHandle(`${id}.vstate`, false);
}

/** Writes `plan`'s ranges out of `memory` into `file` (synchronous: the
 * machine stays frozen). Returns the bytes written. */
export function writeState(file: FileSystemSyncAccessHandle, memory: ArrayBufferLike, plan: Plan): number {
  file.truncate(0);
  const header = new DataView(new ArrayBuffer(HEADER_BYTES));
  header.setUint32(0, MAGIC, true);
  header.setUint32(4, FORMAT_VERSION, true);
  header.setUint32(8, plan.raw.byteLength, true);
  header.setUint32(12, CHUNK_BYTES, true);
  let at = 0;
  const put = (bytes: Uint8Array): void => {
    at += file.write(bytes, { at });
  };
  put(new Uint8Array(header.buffer));
  put(plan.raw);
  /* Bitmaps first (computed in a pass over memory), then the chunks. */
  const bitmaps = plan.ranges.map((range) => {
    const bitmap = new Uint8Array(Math.ceil(chunkCount(range.bytes) / 8));
    for (let c = 0; c < chunkCount(range.bytes); c++) {
      const start = range.address + c * CHUNK_BYTES;
      const length = Math.min(CHUNK_BYTES, range.bytes - c * CHUNK_BYTES);
      if (!isZero(new Uint8Array(memory, start, length))) bitmap[c >> 3] = (bitmap[c >> 3] ?? 0) | (1 << (c & 7));
    }
    return bitmap;
  });
  for (const bitmap of bitmaps) {
    const length = new DataView(new ArrayBuffer(4));
    length.setUint32(0, bitmap.byteLength, true);
    put(new Uint8Array(length.buffer));
    put(bitmap);
  }
  const scratch = new Uint8Array(CHUNK_BYTES); /* writes take unshared memory */
  plan.ranges.forEach((range, r) => {
    const bitmap = bitmaps[r] as Uint8Array;
    for (let c = 0; c < chunkCount(range.bytes); c++) {
      if (!((bitmap[c >> 3] ?? 0) & (1 << (c & 7)))) continue;
      const length = Math.min(CHUNK_BYTES, range.bytes - c * CHUNK_BYTES);
      scratch.set(new Uint8Array(memory, range.address + c * CHUNK_BYTES, length));
      put(scratch.subarray(0, length));
    }
  });
  file.flush();
  return at;
}

/** The plan stored in `file`, or null if it is not a state file. */
export function readStoredPlan(file: FileSystemSyncAccessHandle): Plan | null {
  const header = new Uint8Array(HEADER_BYTES);
  if (file.read(header, { at: 0 }) !== HEADER_BYTES) return null;
  const v = new DataView(header.buffer);
  if (v.getUint32(0, true) !== MAGIC || v.getUint32(4, true) !== FORMAT_VERSION) return null;
  const planBytes = v.getUint32(8, true);
  if (v.getUint32(12, true) !== CHUNK_BYTES || planBytes !== PLAN_BYTES) return null;
  const raw = new Uint8Array(planBytes);
  if (file.read(raw, { at: HEADER_BYTES }) !== planBytes) return null;
  return parsePlan(raw);
}

/** Writes the stored ranges back into `memory`: stored chunks are read,
 * missing ones zeroed, and what the current timeline used beyond the
 * stored extent (`now`) is zeroed too. Synchronous; false on a short read
 * (memory is then partly written: the caller must not resume it as is). */
export function readState(file: FileSystemSyncAccessHandle, memory: ArrayBufferLike, stored: Plan, now: Plan): boolean {
  let at = HEADER_BYTES + PLAN_BYTES;
  const bitmaps: Uint8Array[] = [];
  for (const range of stored.ranges) {
    const length = new Uint8Array(4);
    if (file.read(length, { at }) !== 4) return false;
    at += 4;
    const bytes = new DataView(length.buffer).getUint32(0, true);
    if (bytes !== Math.ceil(chunkCount(range.bytes) / 8)) return false;
    const bitmap = new Uint8Array(bytes);
    if (file.read(bitmap, { at }) !== bytes) return false;
    at += bytes;
    bitmaps.push(bitmap);
  }
  const scratch = new Uint8Array(CHUNK_BYTES);
  for (let r = 0; r < stored.ranges.length; r++) {
    const range = stored.ranges[r] as PlanRange;
    const bitmap = bitmaps[r] as Uint8Array;
    for (let c = 0; c < chunkCount(range.bytes); c++) {
      const length = Math.min(CHUNK_BYTES, range.bytes - c * CHUNK_BYTES);
      const target = new Uint8Array(memory, range.address + c * CHUNK_BYTES, length);
      if ((bitmap[c >> 3] ?? 0) & (1 << (c & 7))) {
        const chunk = scratch.subarray(0, length);
        if (file.read(chunk, { at }) !== length) return false;
        at += length;
        target.set(chunk);
      } else {
        target.fill(0);
      }
    }
    const later = (now.ranges[r] as PlanRange).bytes;
    if (later > range.bytes) new Uint8Array(memory, range.address + range.bytes, later - range.bytes).fill(0);
  }
  return true;
}

/* ---- the list the UI shows ---------------------------------------- */

export function stateInfo(id: string, titleId: string, plan: Plan, bytes: number, createdAt: number): SavestateInfo {
  return { id, titleId, createdAt, virtualSeconds: plan.virtualTicks / TIMER_HZ, bytes };
}

export async function writeStateInfo(info: SavestateInfo): Promise<void> {
  const dir = await stateDirectory();
  const writable = await (await dir.getFileHandle(`${info.id}.json`, { create: true })).createWritable();
  await writable.write(JSON.stringify(info));
  await writable.close();
}

function isInfo(value: unknown): value is SavestateInfo {
  if (typeof value !== "object" || value === null) return false;
  const v = value as Record<string, unknown>;
  return typeof v["id"] === "string" && typeof v["titleId"] === "string" && typeof v["createdAt"] === "number" &&
    typeof v["virtualSeconds"] === "number" && typeof v["bytes"] === "number";
}

/** Every stored state, newest first. */
export async function listStates(): Promise<readonly SavestateInfo[]> {
  if (typeof navigator === "undefined" || !navigator.storage?.getDirectory) return [];
  const dir = await stateDirectory();
  const states: SavestateInfo[] = [];
  for await (const [name, handle] of dir.entries()) {
    if (handle.kind !== "file" || !name.endsWith(".json")) continue;
    try {
      const parsed: unknown = JSON.parse(await (await (handle as FileSystemFileHandle).getFile()).text());
      if (isInfo(parsed)) states.push(parsed);
    } catch {
      /* a damaged listing: skipped */
    }
  }
  return states.sort((a, b) => b.createdAt - a.createdAt);
}

export async function deleteState(id: string): Promise<void> {
  const dir = await stateDirectory();
  await dir.removeEntry(`${id}.vstate`).catch(() => undefined);
  await dir.removeEntry(`${id}.json`).catch(() => undefined);
}

/** A state id: the title and the moment, safe as an OPFS name. Pure. */
export function newStateId(titleId: string, now: number): string {
  return `${titleId}-${now.toString(36)}`;
}
