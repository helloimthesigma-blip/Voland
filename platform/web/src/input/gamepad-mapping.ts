/**
 * Gamepad API -> input-region state (§18 "Controller mapping"). The
 * Standard Gamepad layout is positional (Xbox-style: 0 = bottom face
 * button); Switch labels are swapped against it (A is the RIGHT face
 * button), so the remap happens here, once - the core and the hid:
 * writer only ever see Switch bits.
 */
import { type ControllerState, INPUT_AXIS_COUNT, INPUT_AXIS_MAX, InputButton, InputDeviceKind } from "./input-region.ts";

/** Structural subset of `Gamepad`, so tests need no browser. */
export interface GamepadLike {
  readonly connected: boolean;
  readonly mapping: string;
  readonly buttons: readonly { readonly pressed: boolean }[];
  readonly axes: readonly number[];
}

export const STANDARD_PROFILE_ID = 0;

/** [Switch button bit, standard-gamepad button index]. */
const STANDARD_BUTTON_MAP: readonly (readonly [number, number])[] = [
  [InputButton.A, 1],        // right face
  [InputButton.B, 0],        // bottom face
  [InputButton.X, 3],        // top face
  [InputButton.Y, 2],        // left face
  [InputButton.L, 4],
  [InputButton.R, 5],
  [InputButton.ZL, 6],
  [InputButton.ZR, 7],
  [InputButton.Minus, 8],
  [InputButton.Plus, 9],
  [InputButton.StickL, 10],
  [InputButton.StickR, 11],
  [InputButton.DpadUp, 12],
  [InputButton.DpadDown, 13],
  [InputButton.DpadLeft, 14],
  [InputButton.DpadRight, 15],
  [InputButton.Home, 16],
];

/** Gamepad API axis in [-1, 1] -> Horizon stick units, optionally inverted. */
export function toStickUnits(value: number, invert: boolean): number {
  const clamped = Math.max(-1, Math.min(1, Number.isFinite(value) ? value : 0));
  // Symmetric rounding: Math.round alone rounds -x.5 toward +infinity,
  // which would make equal left/right deflections differ by one unit.
  const scaled = Math.sign(clamped) * Math.round(Math.abs(clamped) * INPUT_AXIS_MAX);
  return (invert ? -scaled : scaled) + 0; // + 0 folds -0 into 0
}

export function mapStandardGamepad(pad: GamepadLike): ControllerState {
  let buttons = 0;
  for (const [bit, index] of STANDARD_BUTTON_MAP) {
    if (pad.buttons[index]?.pressed) buttons |= bit;
  }
  const axes = Array.from({ length: INPUT_AXIS_COUNT }, () => 0);
  // Gamepad API: +Y is down. Horizon: +Y is up.
  axes[0] = toStickUnits(pad.axes[0] ?? 0, false);
  axes[1] = toStickUnits(pad.axes[1] ?? 0, true);
  axes[2] = toStickUnits(pad.axes[2] ?? 0, false);
  axes[3] = toStickUnits(pad.axes[3] ?? 0, true);
  return {
    buttons: buttons >>> 0,
    axes,
    connected: pad.connected,
    deviceKind: InputDeviceKind.StandardGamepad,
    profileId: STANDARD_PROFILE_ID,
  };
}
