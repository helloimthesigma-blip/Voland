/**
 * Web writer for the §18 input region - the main-thread half of the
 * seqlock whose reader is core/common/input_region.c. Mirrors that
 * header's slot layout exactly; tests/data/input_region_vectors.txt pins
 * both writers to the same bytes.
 *
 * Per slot, once per rAF: Atomics.add(sequence, 1) -> payload ->
 * Atomics.add(sequence, 1). Atomics are sequentially consistent, which
 * subsumes the release ordering the protocol needs. The region lives in
 * the single shared WebAssembly.Memory (§4); views are made once and are
 * valid forever because the memory never grows.
 */

export const INPUT_REGION_SLOT_COUNT = 8;
export const INPUT_REGION_SLOT_BYTES = 32;
export const INPUT_AXIS_COUNT = 8;
export const INPUT_AXIS_MAX = 32767;

const OFFSET_SEQUENCE = 0;
const OFFSET_BUTTONS = 4;
const OFFSET_AXES = 8;
const OFFSET_FLAGS = 24;
const OFFSET_RESERVED = 28;
const AXIS_BYTES = 2;

/** HidNpadButton-compatible bits (input_region.h INPUT_BUTTON_*). */
export const InputButton = {
  A: 1 << 0,
  B: 1 << 1,
  X: 1 << 2,
  Y: 1 << 3,
  StickL: 1 << 4,
  StickR: 1 << 5,
  L: 1 << 6,
  R: 1 << 7,
  ZL: 1 << 8,
  ZR: 1 << 9,
  Plus: 1 << 10,
  Minus: 1 << 11,
  DpadLeft: 1 << 12,
  DpadUp: 1 << 13,
  DpadRight: 1 << 14,
  DpadDown: 1 << 15,
  Home: 1 << 28,
  Capture: 1 << 29,
} as const;

export const InputAxis = { LeftX: 0, LeftY: 1, RightX: 2, RightY: 3 } as const;

/** input_region.h Input_Device_Kind. */
export const InputDeviceKind = {
  None: 0,
  StandardGamepad: 1,
  ProController: 2,
  JoyConLeft: 3,
  JoyConRight: 4,
  Keyboard: 5,
} as const;

const FLAG_CONNECTED = 1;
const FLAG_DEVICE_KIND_SHIFT = 8;
const FLAG_PROFILE_ID_SHIFT = 16;
const DEVICE_KIND_MASK = 0xff;
const PROFILE_ID_MASK = 0xffff;

export interface ControllerState {
  readonly buttons: number;
  /** INPUT_AXIS_COUNT values in [-32767, 32767], +Y = up. */
  readonly axes: readonly number[];
  readonly connected: boolean;
  readonly deviceKind: number;
  readonly profileId: number;
}

export const DISCONNECTED: ControllerState = {
  buttons: 0,
  axes: [0, 0, 0, 0, 0, 0, 0, 0],
  connected: false,
  deviceKind: InputDeviceKind.None,
  profileId: 0,
};

export function packFlags(state: ControllerState): number {
  return ((state.connected ? FLAG_CONNECTED : 0) |
          ((state.deviceKind & DEVICE_KIND_MASK) << FLAG_DEVICE_KIND_SHIFT) |
          ((state.profileId & PROFILE_ID_MASK) << FLAG_PROFILE_ID_SHIFT)) >>> 0;
}

/** Touch block after the slots (input_region.h INPUT_TOUCH_*). */
export const INPUT_TOUCH_MAX = 2;
export const INPUT_TOUCH_WIDTH = 1280;
export const INPUT_TOUCH_HEIGHT = 720;
const TOUCH_BLOCK_BYTES = 32;
const TOUCH_OFFSET_COUNT = 4;
const TOUCH_OFFSET_POINTS = 8;
const TOUCH_POINT_BYTES = 4;

export interface TouchPoint {
  readonly x: number;
  readonly y: number;
}

export interface InputRegionWriter {
  writeSlot(slot: number, state: ControllerState): void;
  /** The touch screen: up to INPUT_TOUCH_MAX points in 1280x720 space. */
  writeTouch(points: readonly TouchPoint[]): void;
}

/** `regionBase` is layout.inputRegionBase as a byte offset (toByteOffset). */
export function createInputRegionWriter(buffer: ArrayBufferLike, regionBase: number): InputRegionWriter {
  const bytes = INPUT_REGION_SLOT_COUNT * INPUT_REGION_SLOT_BYTES;
  const view = new DataView(buffer, regionBase, bytes);
  const words = new Int32Array(buffer, regionBase, bytes / Int32Array.BYTES_PER_ELEMENT);
  const touchView = new DataView(buffer, regionBase + bytes, TOUCH_BLOCK_BYTES);
  const touchWords = new Int32Array(buffer, regionBase + bytes, 1);
  const clamp = (value: number, max: number): number => Math.min(max - 1, Math.max(0, Math.round(value)));

  return {
    writeSlot(slot: number, state: ControllerState): void {
      if (!Number.isInteger(slot) || slot < 0 || slot >= INPUT_REGION_SLOT_COUNT) {
        throw new RangeError(`input slot ${slot} out of range`);
      }
      const base = slot * INPUT_REGION_SLOT_BYTES;
      const sequenceIndex = (base + OFFSET_SEQUENCE) / Int32Array.BYTES_PER_ELEMENT;
      Atomics.add(words, sequenceIndex, 1); // odd: write in progress
      view.setUint32(base + OFFSET_BUTTONS, state.buttons >>> 0, true);
      for (let axis = 0; axis < INPUT_AXIS_COUNT; axis++) {
        view.setInt16(base + OFFSET_AXES + axis * AXIS_BYTES, state.axes[axis] ?? 0, true);
      }
      view.setUint32(base + OFFSET_FLAGS, packFlags(state), true);
      view.setUint32(base + OFFSET_RESERVED, 0, true);
      Atomics.add(words, sequenceIndex, 1); // even: published
    },
    writeTouch(points: readonly TouchPoint[]): void {
      const count = Math.min(points.length, INPUT_TOUCH_MAX);
      Atomics.add(touchWords, 0, 1); // odd: write in progress
      touchView.setUint32(TOUCH_OFFSET_COUNT, count, true);
      for (let i = TOUCH_OFFSET_POINTS; i < TOUCH_BLOCK_BYTES; i += TOUCH_POINT_BYTES) touchView.setUint32(i, 0, true);
      points.slice(0, count).forEach((point, i) => {
        const at = TOUCH_OFFSET_POINTS + i * TOUCH_POINT_BYTES;
        touchView.setUint16(at, clamp(point.x, INPUT_TOUCH_WIDTH), true);
        touchView.setUint16(at + 2, clamp(point.y, INPUT_TOUCH_HEIGHT), true);
      });
      Atomics.add(touchWords, 0, 1); // even: published
    },
  };
}
