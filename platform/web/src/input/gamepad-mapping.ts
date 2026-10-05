/**
 * Gamepad API -> input-region state (§18 "Controller mapping"). The
 * Standard Gamepad layout is positional (Xbox-style: 0 = bottom face
 * button); Switch labels are swapped against it (A is the RIGHT face
 * button), so the remap happens here, once, through the player's
 * bindings (bindings.ts) - the core and the hid: writer only ever see
 * Switch bits.
 */
import { CONTROLS, DEFAULT_PAD_BINDINGS, type PadBindings } from "./bindings.ts";
import { type ControllerState, INPUT_AXIS_COUNT, INPUT_AXIS_MAX, InputDeviceKind } from "./input-region.ts";

/** Structural subset of `Gamepad`, so tests need no browser. */
export interface GamepadLike {
  readonly connected: boolean;
  readonly mapping: string;
  readonly buttons: readonly { readonly pressed: boolean }[];
  readonly axes: readonly number[];
}

export const STANDARD_PROFILE_ID = 0;

/** Gamepad API axis in [-1, 1] -> Horizon stick units, optionally inverted. */
export function toStickUnits(value: number, invert: boolean): number {
  const clamped = Math.max(-1, Math.min(1, Number.isFinite(value) ? value : 0));
  // Symmetric rounding: Math.round alone rounds -x.5 toward +infinity,
  // which would make equal left/right deflections differ by one unit.
  const scaled = Math.sign(clamped) * Math.round(Math.abs(clamped) * INPUT_AXIS_MAX);
  return (invert ? -scaled : scaled) + 0; // + 0 folds -0 into 0
}

export function mapStandardGamepad(pad: GamepadLike, bindings: PadBindings = DEFAULT_PAD_BINDINGS): ControllerState {
  let buttons = 0;
  for (const control of CONTROLS) {
    if (control.kind !== "button") continue;
    if ((bindings[control.id] ?? []).some((index) => pad.buttons[index]?.pressed)) buttons |= control.bit;
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
