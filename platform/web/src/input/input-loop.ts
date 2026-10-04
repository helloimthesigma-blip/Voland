/**
 * Main-thread input pump (§18): once per animation frame, read every
 * source and write the input region. Nothing per-frame crosses
 * postMessage (§6); only connect/disconnect is reported, as a lifecycle
 * event (§16). rAF stops in hidden tabs, which is fine - the CPU worker
 * is paused by the visibility handler in main.ts at the same time.
 *
 * Slot policy: slot N is navigator.getGamepads()[N]. The keyboard drives
 * slot 0 while no gamepad occupies it - connected from the start, as a
 * Switch always has its handheld controller (games that find none keep
 * asking for one with the controller applet, and stay silent meanwhile).
 *
 * Short taps: a press shows at once, but its release waits until the
 * press has lasted a minimum hold (two guest frames at the measured frame
 * rate), so a tap between two of the game's input reads still registers.
 */
import {
  type ControllerState,
  DISCONNECTED,
  INPUT_REGION_SLOT_COUNT,
  INPUT_TOUCH_HEIGHT,
  INPUT_TOUCH_WIDTH,
  type TouchPoint,
  createInputRegionWriter,
} from "./input-region.ts";
import { type GamepadLike, mapStandardGamepad } from "./gamepad-mapping.ts";
import { isMappedKey, mapKeyboard } from "./keyboard-mapping.ts";

export interface ConnectionChange {
  readonly slot: number;
  readonly connected: boolean;
  readonly profileId: number;
}

/* Minimum hold, as guest frames, and its bounds (ms). */
const HOLD_FRAMES = 2;
const HOLD_MIN_MS = 40;
const HOLD_MAX_MS = 300;
const BUTTON_BITS = 32;

/** The minimum hold for taps (ms) at a guest frame rate (0 = unknown). Pure. */
export function minimumHoldMs(fps: number): number {
  if (!(fps > 0)) return HOLD_MAX_MS;
  return Math.max(HOLD_MIN_MS, Math.min(HOLD_MAX_MS, (HOLD_FRAMES * 1000) / fps));
}

/** When each held button bit of a slot was pressed (ms), or absent. */
export type HoldState = ReadonlyMap<number, number>;

/** Buttons as the guest should see them: presses at once, releases only
 * once the press has lasted `holdMs`. Pure. */
export function applyMinimumHold(state: HoldState, buttons: number, now: number, holdMs: number):
  { buttons: number; state: HoldState } {
  const next = new Map<number, number>();
  let out = buttons;
  for (let bit = 0; bit < BUTTON_BITS; bit++) {
    const mask = (1 << bit) >>> 0;
    const pressedAt = state.get(bit);
    if (buttons & mask) {
      next.set(bit, pressedAt ?? now);
    } else if (pressedAt !== undefined && now - pressedAt < holdMs) {
      out |= mask; /* released too soon: keep it down a little longer */
      next.set(bit, pressedAt);
    }
  }
  return { buttons: out >>> 0, state: next };
}

export interface InputLoopOptions {
  readonly buffer: ArrayBufferLike;
  readonly regionBase: number;
  readonly getGamepads: () => readonly (GamepadLike | null)[];
  readonly onConnectionChange: (change: ConnectionChange) => void;
  /** The element showing the guest screen; pointer presses on it are
   * the touch screen. */
  readonly touchTarget?: HTMLElement;
  /** The guest's frames per second (for the tap minimum hold); 0 = unknown. */
  readonly guestFps?: () => number;
}

export interface ScreenBox {
  readonly left: number;
  readonly top: number;
  readonly width: number;
  readonly height: number;
}

/**
 * A client-space point on `box` (an element showing the 16:9 screen with
 * object-fit: contain) in 1280x720 touch coordinates, or null when it
 * falls on the letterbox. Pure.
 */
export function mapPointerToTouch(clientX: number, clientY: number, box: ScreenBox): TouchPoint | null {
  if (box.width <= 0 || box.height <= 0) return null;
  const scale = Math.min(box.width / INPUT_TOUCH_WIDTH, box.height / INPUT_TOUCH_HEIGHT);
  const left = box.left + (box.width - INPUT_TOUCH_WIDTH * scale) / 2;
  const top = box.top + (box.height - INPUT_TOUCH_HEIGHT * scale) / 2;
  const x = (clientX - left) / scale;
  const y = (clientY - top) / scale;
  if (x < 0 || y < 0 || x >= INPUT_TOUCH_WIDTH || y >= INPUT_TOUCH_HEIGHT) return null;
  return { x: Math.floor(x), y: Math.floor(y) };
}

/** One frame's worth of slot states from the current sources. Pure. */
export function collectSlotStates(
  gamepads: readonly (GamepadLike | null)[],
  keyboardActive: boolean,
  pressedKeys: ReadonlySet<string>,
): readonly ControllerState[] {
  return Array.from({ length: INPUT_REGION_SLOT_COUNT }, (_, slot) => {
    const pad = gamepads[slot];
    if (pad && pad.connected) return mapStandardGamepad(pad);
    if (slot === 0 && keyboardActive) return mapKeyboard(pressedKeys);
    return DISCONNECTED;
  });
}

function isTextEntry(target: EventTarget | null): boolean {
  return target instanceof HTMLElement &&
    (target.isContentEditable || target.tagName === "INPUT" || target.tagName === "TEXTAREA");
}

/** Starts the pump; returns a stop function. */
export function startInputLoop(options: InputLoopOptions): () => void {
  const writer = createInputRegionWriter(options.buffer, options.regionBase);
  const pressedKeys = new Set<string>();
  const keyboardActive = true;
  let previous: readonly ControllerState[] = Array.from({ length: INPUT_REGION_SLOT_COUNT }, () => DISCONNECTED);
  let frame = 0;
  const holds: HoldState[] = Array.from({ length: INPUT_REGION_SLOT_COUNT }, () => new Map<number, number>());

  const onKeyDown = (event: KeyboardEvent): void => {
    if (isTextEntry(event.target) || !isMappedKey(event.code)) return;
    pressedKeys.add(event.code);
    event.preventDefault();
  };
  const onKeyUp = (event: KeyboardEvent): void => {
    pressedKeys.delete(event.code);
  };
  const onBlur = (): void => pressedKeys.clear(); // no stuck keys after alt-tab

  window.addEventListener("keydown", onKeyDown);
  window.addEventListener("keyup", onKeyUp);
  window.addEventListener("blur", onBlur);

  // Touch screen: pointers pressed on the screen, in press order.
  const pointers = new Map<number, TouchPoint>();
  const target = options.touchTarget;
  const onPointer = (event: PointerEvent): void => {
    if (!target) return;
    if (event.type === "pointerdown" && event.button !== 0) return;
    if (event.type !== "pointerdown" && !pointers.has(event.pointerId)) return;
    const point = mapPointerToTouch(event.clientX, event.clientY, target.getBoundingClientRect());
    if (event.type === "pointerdown") {
      if (!point) return;
      target.setPointerCapture(event.pointerId);
      pointers.set(event.pointerId, point);
      event.preventDefault();
    } else if (point) {
      pointers.set(event.pointerId, point);
    }
  };
  const onPointerEnd = (event: PointerEvent): void => {
    pointers.delete(event.pointerId);
  };
  if (target) {
    target.style.touchAction = "none"; // no browser panning/zoom over the screen
    target.addEventListener("pointerdown", onPointer);
    target.addEventListener("pointermove", onPointer);
    target.addEventListener("pointerup", onPointerEnd);
    target.addEventListener("pointercancel", onPointerEnd);
  }

  const tick = (): void => {
    const now = performance.now();
    const holdMs = minimumHoldMs(options.guestFps?.() ?? 0);
    const states = collectSlotStates(options.getGamepads(), keyboardActive, pressedKeys).map((state, slot) => {
      const held = applyMinimumHold(holds[slot] ?? new Map(), state.buttons, now, holdMs);
      holds[slot] = held.state;
      return held.buttons === state.buttons ? state : { ...state, buttons: held.buttons };
    });
    states.forEach((state, slot) => {
      writer.writeSlot(slot, state);
      const before = previous[slot];
      if (before && (before.connected !== state.connected || before.profileId !== state.profileId)) {
        options.onConnectionChange({ slot, connected: state.connected, profileId: state.profileId });
      }
    });
    previous = states;
    writer.writeTouch(Array.from(pointers.values()));
    frame = requestAnimationFrame(tick);
  };
  frame = requestAnimationFrame(tick);

  return () => {
    cancelAnimationFrame(frame);
    window.removeEventListener("keydown", onKeyDown);
    window.removeEventListener("keyup", onKeyUp);
    window.removeEventListener("blur", onBlur);
    if (target) {
      target.removeEventListener("pointerdown", onPointer);
      target.removeEventListener("pointermove", onPointer);
      target.removeEventListener("pointerup", onPointerEnd);
      target.removeEventListener("pointercancel", onPointerEnd);
    }
  };
}
