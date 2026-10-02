/**
 * Software-keyboard requests from the core (emulator_text_request_ffi):
 * the fields arrive as one string joined by U+001F.
 */
import type { TextInputRequest } from "@bindings/protocol";

const SEPARATOR = "\u001f";

export function parseTextInputRequest(raw: string): TextInputRequest | null {
  const parts = raw.split(SEPARATOR);
  if (parts.length < 7) return null;
  const [header = "", sub = "", guide = "", initial = "", max = "0", min = "0", password = "0"] = parts;
  return {
    header,
    sub,
    guide,
    initial,
    maxLength: Number.parseInt(max, 10) || 0,
    minLength: Number.parseInt(min, 10) || 0,
    password: password === "1",
  };
}
