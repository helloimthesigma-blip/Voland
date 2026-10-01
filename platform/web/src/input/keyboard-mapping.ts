/**
 * Default keyboard profile (§18 "Sources": keyboard/pointer). Keyed on
 * KeyboardEvent.code (physical position), so it is layout-independent.
 * Sticks are digital on a keyboard: full deflection per direction.
 */
import { type ControllerState, INPUT_AXIS_COUNT, INPUT_AXIS_MAX, InputButton, InputDeviceKind } from "./input-region.ts";

export const KEYBOARD_PROFILE_ID = 1;

export const KEYBOARD_BUTTON_MAP: Readonly<Record<string, number>> = {
  KeyZ: InputButton.A,
  KeyX: InputButton.B,
  KeyC: InputButton.X,
  KeyV: InputButton.Y,
  KeyE: InputButton.L,
  KeyU: InputButton.R,
  KeyQ: InputButton.ZL,
  KeyO: InputButton.ZR,
  Minus: InputButton.Minus,
  Equal: InputButton.Plus,
  KeyF: InputButton.StickL,
  KeyN: InputButton.StickR,
  ArrowUp: InputButton.DpadUp,
  ArrowDown: InputButton.DpadDown,
  ArrowLeft: InputButton.DpadLeft,
  ArrowRight: InputButton.DpadRight,
};

/** [code, axis index, direction]. */
const KEYBOARD_STICK_MAP: readonly (readonly [string, number, 1 | -1])[] = [
  ["KeyW", 1, 1], ["KeyS", 1, -1], ["KeyA", 0, -1], ["KeyD", 0, 1],
  ["KeyI", 3, 1], ["KeyK", 3, -1], ["KeyJ", 2, -1], ["KeyL", 2, 1],
];

export function isMappedKey(code: string): boolean {
  return code in KEYBOARD_BUTTON_MAP || KEYBOARD_STICK_MAP.some(([key]) => key === code);
}

export function mapKeyboard(pressed: ReadonlySet<string>): ControllerState {
  let buttons = 0;
  for (const [code, bit] of Object.entries(KEYBOARD_BUTTON_MAP)) {
    if (pressed.has(code)) buttons |= bit;
  }
  const axes = Array.from({ length: INPUT_AXIS_COUNT }, () => 0);
  for (const [code, axis, direction] of KEYBOARD_STICK_MAP) {
    if (pressed.has(code)) axes[axis] = (axes[axis] ?? 0) + direction * INPUT_AXIS_MAX;
  }
  return {
    buttons: buttons >>> 0,
    axes: axes.map((value) => Math.max(-INPUT_AXIS_MAX, Math.min(INPUT_AXIS_MAX, value))),
    connected: true,
    deviceKind: InputDeviceKind.Keyboard,
    profileId: KEYBOARD_PROFILE_ID,
  };
}
