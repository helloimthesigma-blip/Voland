/**
 * Main-thread input pump (§18): once per animation frame, read every
 * source and write the input region. Nothing per-frame crosses
 * postMessage (§6); only connect/disconnect is reported, as a lifecycle
 * event (§16). rAF stops in hidden tabs, which is fine - the CPU worker
 * is paused by the visibility handler in main.ts at the same time.
 *
 * Slot policy: slot N is navigator.getGamepads()[N]. The keyboard drives
 * slot 0 while no gamepad occupies it, from the first mapped key press.
 */
import {
  type ControllerState,
  DISCONNECTED,
  INPUT_REGION_SLOT_COUNT,
  createInputRegionWriter,
} from "./input-region.ts";
import { type GamepadLike, mapStandardGamepad } from "./gamepad-mapping.ts";
import { isMappedKey, mapKeyboard } from "./keyboard-mapping.ts";

export interface ConnectionChange {
  readonly slot: number;
  readonly connected: boolean;
  readonly profileId: number;
}

export interface InputLoopOptions {
  readonly buffer: ArrayBufferLike;
  readonly regionBase: number;
  readonly getGamepads: () => readonly (GamepadLike | null)[];
  readonly onConnectionChange: (change: ConnectionChange) => void;
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
  let keyboardActive = false;
  let previous: readonly ControllerState[] = Array.from({ length: INPUT_REGION_SLOT_COUNT }, () => DISCONNECTED);
  let frame = 0;

  const onKeyDown = (event: KeyboardEvent): void => {
    if (isTextEntry(event.target) || !isMappedKey(event.code)) return;
    keyboardActive = true;
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

  const tick = (): void => {
    const states = collectSlotStates(options.getGamepads(), keyboardActive, pressedKeys);
    states.forEach((state, slot) => {
      writer.writeSlot(slot, state);
      const before = previous[slot];
      if (before && (before.connected !== state.connected || before.profileId !== state.profileId)) {
        options.onConnectionChange({ slot, connected: state.connected, profileId: state.profileId });
      }
    });
    previous = states;
    frame = requestAnimationFrame(tick);
  };
  frame = requestAnimationFrame(tick);

  return () => {
    cancelAnimationFrame(frame);
    window.removeEventListener("keydown", onKeyDown);
    window.removeEventListener("keyup", onKeyUp);
    window.removeEventListener("blur", onBlur);
  };
}
