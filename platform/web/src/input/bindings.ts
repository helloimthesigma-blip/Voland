/**
 * Remappable controls (§18 "Controller mapping"): which keyboard keys and
 * which standard-gamepad buttons drive each Switch control. The defaults
 * are the layouts the legend has always shown; the player's changes are
 * kept per browser (bindings-store.ts). Pure data and pure functions -
 * every change returns new bindings.
 */
import { InputAxis, InputButton } from "./input-region.ts";

/** A Switch control a binding can drive: a button bit, or one direction
 * of a stick (keyboards have no analog sticks). */
export type Control =
  | { readonly id: string; readonly label: string; readonly kind: "button"; readonly bit: number }
  | { readonly id: string; readonly label: string; readonly kind: "stick"; readonly axis: number; readonly direction: 1 | -1 };

const button = (id: string, label: string, bit: number): Control => ({ id, label, kind: "button", bit });
const stick = (id: string, label: string, axis: number, direction: 1 | -1): Control =>
  ({ id, label, kind: "stick", axis, direction });

/** Every control, in the order the controls panel lists them. */
export const CONTROLS: readonly Control[] = [
  button("A", "A", InputButton.A),
  button("B", "B", InputButton.B),
  button("X", "X", InputButton.X),
  button("Y", "Y", InputButton.Y),
  button("L", "L", InputButton.L),
  button("R", "R", InputButton.R),
  button("ZL", "ZL", InputButton.ZL),
  button("ZR", "ZR", InputButton.ZR),
  button("Plus", "+", InputButton.Plus),
  button("Minus", "−", InputButton.Minus),
  button("DpadUp", "D-pad ↑", InputButton.DpadUp),
  button("DpadDown", "D-pad ↓", InputButton.DpadDown),
  button("DpadLeft", "D-pad ←", InputButton.DpadLeft),
  button("DpadRight", "D-pad →", InputButton.DpadRight),
  stick("LStickUp", "Left stick ↑", InputAxis.LeftY, 1),
  stick("LStickDown", "Left stick ↓", InputAxis.LeftY, -1),
  stick("LStickLeft", "Left stick ←", InputAxis.LeftX, -1),
  stick("LStickRight", "Left stick →", InputAxis.LeftX, 1),
  stick("RStickUp", "Right stick ↑", InputAxis.RightY, 1),
  stick("RStickDown", "Right stick ↓", InputAxis.RightY, -1),
  stick("RStickLeft", "Right stick ←", InputAxis.RightX, -1),
  stick("RStickRight", "Right stick →", InputAxis.RightX, 1),
  button("StickL", "Left stick click", InputButton.StickL),
  button("StickR", "Right stick click", InputButton.StickR),
  button("Home", "Home", InputButton.Home),
];

/** Control id -> KeyboardEvent.code values (physical keys). */
export type KeyBindings = Readonly<Record<string, readonly string[]>>;
/** Control id -> standard-gamepad button indices (sticks stay analog). */
export type PadBindings = Readonly<Record<string, readonly number[]>>;

export interface Bindings {
  readonly keyboard: KeyBindings;
  readonly gamepad: PadBindings;
}

export const DEFAULT_KEY_BINDINGS: KeyBindings = {
  A: ["KeyZ"], B: ["KeyX"], X: ["KeyC"], Y: ["KeyV"],
  L: ["KeyE"], R: ["KeyU"], ZL: ["KeyQ"], ZR: ["KeyO"],
  Plus: ["Equal"], Minus: ["Minus"],
  DpadUp: ["ArrowUp"], DpadDown: ["ArrowDown"], DpadLeft: ["ArrowLeft"], DpadRight: ["ArrowRight"],
  LStickUp: ["KeyW"], LStickDown: ["KeyS"], LStickLeft: ["KeyA"], LStickRight: ["KeyD"],
  RStickUp: ["KeyI"], RStickDown: ["KeyK"], RStickLeft: ["KeyJ"], RStickRight: ["KeyL"],
  StickL: ["KeyF"], StickR: ["KeyN"], Home: [],
};

/* Standard Gamepad layout is positional (Xbox-style: 0 = bottom face);
 * Switch labels are swapped against it (A is the RIGHT face button). */
export const DEFAULT_PAD_BINDINGS: PadBindings = {
  A: [1], B: [0], X: [3], Y: [2],
  L: [4], R: [5], ZL: [6], ZR: [7],
  Minus: [8], Plus: [9], StickL: [10], StickR: [11],
  DpadUp: [12], DpadDown: [13], DpadLeft: [14], DpadRight: [15],
  Home: [16],
};

export const DEFAULT_BINDINGS: Bindings = { keyboard: DEFAULT_KEY_BINDINGS, gamepad: DEFAULT_PAD_BINDINGS };

/** Controls a gamepad button can be bound to (sticks are the pad's own). */
export function isPadBindable(control: Control): boolean {
  return control.kind === "button";
}

function without<T>(table: Readonly<Record<string, readonly T[]>>, value: T): Record<string, readonly T[]> {
  return Object.fromEntries(Object.entries(table).map(([id, values]) => [id, values.filter((v) => v !== value)]));
}

/** `code` now drives `controlId` only (it is taken off any other control).
 * Pure. */
export function bindKey(bindings: Bindings, controlId: string, code: string): Bindings {
  return { ...bindings, keyboard: { ...without(bindings.keyboard, code), [controlId]: [code] } };
}

/** Gamepad button `index` now drives `controlId` only. Pure. */
export function bindPadButton(bindings: Bindings, controlId: string, index: number): Bindings {
  return { ...bindings, gamepad: { ...without(bindings.gamepad, index), [controlId]: [index] } };
}

/** `controlId` has no key / no pad button. Pure. */
export function clearKey(bindings: Bindings, controlId: string): Bindings {
  return { ...bindings, keyboard: { ...bindings.keyboard, [controlId]: [] } };
}
export function clearPadButton(bindings: Bindings, controlId: string): Bindings {
  return { ...bindings, gamepad: { ...bindings.gamepad, [controlId]: [] } };
}

/** Keeps only well-formed entries for known controls; missing controls get
 * their defaults (so bindings saved by an older version still load). Pure. */
export function sanitizeBindings(value: unknown): Bindings {
  const record = (v: unknown): Readonly<Record<string, unknown>> =>
    typeof v === "object" && v !== null ? (v as Record<string, unknown>) : {};
  const raw = record(value);
  const keys = record(raw["keyboard"]);
  const pads = record(raw["gamepad"]);
  const keyboard: Record<string, readonly string[]> = {};
  const gamepad: Record<string, readonly number[]> = {};
  for (const control of CONTROLS) {
    const k = keys[control.id];
    keyboard[control.id] = Array.isArray(k)
      ? k.filter((c): c is string => typeof c === "string" && c.length > 0 && c.length < 40)
      : (DEFAULT_KEY_BINDINGS[control.id] ?? []);
    if (!isPadBindable(control)) continue;
    const p = pads[control.id];
    gamepad[control.id] = Array.isArray(p)
      ? p.filter((i): i is number => Number.isInteger(i) && i >= 0 && i < 64)
      : (DEFAULT_PAD_BINDINGS[control.id] ?? []);
  }
  return { keyboard, gamepad };
}
