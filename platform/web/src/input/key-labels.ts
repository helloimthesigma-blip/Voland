/**
 * Key-cap labels for KeyboardEvent.code values (the controls legend):
 * "KeyZ" -> "Z", "ArrowUp" -> "↑", "Minus" -> "-".
 */
const SPECIAL_LABELS: Readonly<Record<string, string>> = {
  ArrowUp: "↑",
  ArrowDown: "↓",
  ArrowLeft: "←",
  ArrowRight: "→",
  Minus: "-",
  Equal: "=",
};

export function keyLabel(code: string): string {
  if (code.startsWith("Key")) return code.slice(3);
  return SPECIAL_LABELS[code] ?? code;
}
