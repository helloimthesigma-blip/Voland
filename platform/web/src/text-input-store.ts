/**
 * The software-keyboard prompt the shell is showing, if any (§12 library
 * applets). Fed by main.ts from the CPU worker's text-input-request; read
 * by the dialog. Replaced, never mutated (§3).
 */
import type { TextInputRequest } from "@bindings/protocol";

let current: TextInputRequest | null = null;
const subscribers = new Set<(next: TextInputRequest | null) => void>();

function publish(next: TextInputRequest | null): void {
  current = next;
  subscribers.forEach((fn) => fn(current));
}

export function getTextInput(): TextInputRequest | null {
  return current;
}

export function showTextInput(request: TextInputRequest): void {
  publish(request);
}

export function clearTextInput(): void {
  publish(null);
}

export function subscribeTextInput(fn: (next: TextInputRequest | null) => void): () => void {
  subscribers.add(fn);
  return () => subscribers.delete(fn);
}
