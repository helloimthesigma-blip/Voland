/**
 * The keyboard controls, generated from the default keyboard profile
 * (src/input/keyboard-mapping.ts) so the legend can never drift from the
 * mapping. Gamepads use the standard layout (§18).
 */
import { For } from "solid-js";
import { KEYBOARD_BUTTON_MAP, KEYBOARD_STICK_KEYS } from "../input/keyboard-mapping";
import { InputButton } from "../input/input-region";
import { keyLabel } from "../input/key-labels";

const BUTTON_LABELS: readonly (readonly [number, string])[] = [
  [InputButton.A, "A"], [InputButton.B, "B"], [InputButton.X, "X"], [InputButton.Y, "Y"],
  [InputButton.L, "L"], [InputButton.R, "R"], [InputButton.ZL, "ZL"], [InputButton.ZR, "ZR"],
  [InputButton.Minus, "−"], [InputButton.Plus, "+"],
  [InputButton.StickL, "L-stick click"], [InputButton.StickR, "R-stick click"],
];

function keysFor(bit: number): string {
  return Object.entries(KEYBOARD_BUTTON_MAP)
    .filter(([, mapped]) => mapped === bit)
    .map(([code]) => keyLabel(code))
    .join(" / ");
}

function ControlsLegend() {
  const rows = (): readonly (readonly [string, string])[] => [
    ...BUTTON_LABELS.map(([bit, label]): readonly [string, string] => [label, keysFor(bit)]),
    ["D-pad", "Arrow keys"],
    ["Left stick", KEYBOARD_STICK_KEYS.left.map(keyLabel).join(" ")],
    ["Right stick", KEYBOARD_STICK_KEYS.right.map(keyLabel).join(" ")],
  ];
  return (
    <details class="voland-controls" data-testid="controls">
      <summary>Controls (keyboard; gamepads work too)</summary>
      <p class="voland-controls-hint" data-testid="controls-hint">
        Easiest on a keyboard: move with the arrow keys (the D-pad) and use Z X C V for A B X Y.
        W A S D is the left stick (use it if a game ignores the D-pad for movement). Prompts in games
        show Switch buttons; this list maps them to keys.
      </p>
      <dl>
        <For each={rows()}>
          {([button, keys]) => (
            <div>
              <dt>{button}</dt>
              <dd>{keys}</dd>
            </div>
          )}
        </For>
      </dl>
    </details>
  );
}

export default ControlsLegend;
