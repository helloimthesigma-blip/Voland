/**
 * Key-cap labels for KeyboardEvent.code values (the controls panel):
 * "KeyZ" -> "Z", "ArrowUp" -> "↑", "Digit1" -> "1", "ShiftLeft" ->
 * "Left Shift"; and names for standard-gamepad button indices.
 */
const SPECIAL_LABELS: Readonly<Record<string, string>> = {
  ArrowUp: "↑",
  ArrowDown: "↓",
  ArrowLeft: "←",
  ArrowRight: "→",
  Minus: "-",
  Equal: "=",
  Comma: ",",
  Period: ".",
  Slash: "/",
  Semicolon: ";",
  Quote: "'",
  BracketLeft: "[",
  BracketRight: "]",
  Backslash: "\\",
  Backquote: "`",
};

const SIDES: readonly (readonly [string, string])[] = [["Left", "Left "], ["Right", "Right "]];

export function keyLabel(code: string): string {
  if (code.startsWith("Key")) return code.slice(3);
  if (code.startsWith("Digit")) return code.slice(5);
  if (code.startsWith("Numpad")) return `Num ${code.slice(6)}`;
  for (const [suffix, prefix] of SIDES) {
    if (code.endsWith(suffix) && code.length > suffix.length && !code.startsWith("Arrow")) {
      return `${prefix}${code.slice(0, -suffix.length)}`;
    }
  }
  return SPECIAL_LABELS[code] ?? code;
}

/* Standard Gamepad layout (W3C), by position. */
const PAD_BUTTON_LABELS: readonly string[] = [
  "Bottom face", "Right face", "Left face", "Top face",
  "LB / L1", "RB / R1", "LT / L2", "RT / R2",
  "Select / View", "Start / Menu", "Left stick press", "Right stick press",
  "D-pad ↑", "D-pad ↓", "D-pad ←", "D-pad →", "Home / Guide",
];

export function padButtonLabel(index: number): string {
  return PAD_BUTTON_LABELS[index] ?? `Button ${index}`;
}
