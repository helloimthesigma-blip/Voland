/**
 * Keyboard -> controller state (§18 "Sources": keyboard/pointer). Keyed on
 * KeyboardEvent.code (physical position), so it is layout-independent; the
 * keys come from the player's bindings (bindings.ts; the defaults are Z X
 * C V for A B X Y, arrows for the D-pad, W A S D for the left stick).
 * Sticks are digital on a keyboard: full deflection per direction.
 */
import { CONTROLS, DEFAULT_KEY_BINDINGS, type KeyBindings } from "./bindings.ts";
import { type ControllerState, INPUT_AXIS_COUNT, INPUT_AXIS_MAX, InputDeviceKind } from "./input-region.ts";

export const KEYBOARD_PROFILE_ID = 1;

export function isMappedKey(code: string, keyboard: KeyBindings = DEFAULT_KEY_BINDINGS): boolean {
  return Object.values(keyboard).some((codes) => codes.includes(code));
}

export function mapKeyboard(pressed: ReadonlySet<string>, keyboard: KeyBindings = DEFAULT_KEY_BINDINGS): ControllerState {
  let buttons = 0;
  const axes = Array.from({ length: INPUT_AXIS_COUNT }, () => 0);
  for (const control of CONTROLS) {
    if (!(keyboard[control.id] ?? []).some((code) => pressed.has(code))) continue;
    if (control.kind === "button") buttons |= control.bit;
    else axes[control.axis] = (axes[control.axis] ?? 0) + control.direction * INPUT_AXIS_MAX;
  }
  return {
    buttons: buttons >>> 0,
    axes: axes.map((value) => Math.max(-INPUT_AXIS_MAX, Math.min(INPUT_AXIS_MAX, value))),
    connected: true,
    deviceKind: InputDeviceKind.Keyboard,
    profileId: KEYBOARD_PROFILE_ID,
  };
}
