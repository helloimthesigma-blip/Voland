/**
 * Controls: every Switch control with the keyboard key and gamepad button
 * that drive it, generated from the player's bindings (input/bindings.ts)
 * so the list can never drift from what the input loop does. Click a key
 * or button cell, then press the new key / gamepad button; Esc cancels,
 * Backspace or Delete clears. Kept per browser (input/bindings-store.ts).
 */
import { For, Show, createSignal, onCleanup, onMount } from "solid-js";
import {
  type Bindings, CONTROLS, type Control, DEFAULT_BINDINGS,
  bindKey, bindPadButton, clearKey, clearPadButton, isPadBindable,
} from "../input/bindings";
import { getBindings, setBindings, subscribeBindings } from "../input/bindings-store";
import { keyLabel, padButtonLabel } from "../input/key-labels";

type Listening = { readonly controlId: string; readonly device: "keyboard" | "gamepad" } | null;

const CANCEL_KEY = "Escape";
const CLEAR_KEYS: ReadonlySet<string> = new Set(["Backspace", "Delete"]);

function pressedPadButtons(): ReadonlySet<number> {
  const pressed = new Set<number>();
  for (const pad of navigator.getGamepads?.() ?? []) {
    pad?.buttons.forEach((b, index) => { if (b.pressed) pressed.add(index); });
  }
  return pressed;
}

function ControlsLegend() {
  const [bindings, setLocal] = createSignal<Bindings>(getBindings());
  const [listening, setListening] = createSignal<Listening>(null);
  let padFrame = 0;

  onMount(() => onCleanup(subscribeBindings(setLocal)));

  /* Keyboard capture runs before the input loop's listener (capture
   * phase on window) and swallows the key, so the game never sees it. */
  const onKeyDown = (event: KeyboardEvent): void => {
    const target = listening();
    if (!target) return;
    event.preventDefault();
    event.stopImmediatePropagation();
    const pad = target.device === "gamepad";
    if (event.code === CANCEL_KEY) finish(bindings());
    else if (CLEAR_KEYS.has(event.code)) {
      finish(pad ? clearPadButton(bindings(), target.controlId) : clearKey(bindings(), target.controlId));
    } else if (event.code && !pad) finish(bindKey(bindings(), target.controlId, event.code));
  };
  window.addEventListener("keydown", onKeyDown, true);
  onCleanup(() => {
    window.removeEventListener("keydown", onKeyDown, true);
    cancelAnimationFrame(padFrame);
  });

  function finish(next: Bindings): void {
    setBindings(next);
    setListening(null);
    cancelAnimationFrame(padFrame);
  }

  /* Gamepad capture: the first button that goes down after the click
   * (buttons already held then are ignored until released). */
  function listenForPad(controlId: string): void {
    cancelAnimationFrame(padFrame);
    setListening({ controlId, device: "gamepad" });
    let held = pressedPadButtons();
    const poll = (): void => {
      const target = listening();
      if (!target || target.device !== "gamepad") return;
      const now = pressedPadButtons();
      const fresh = [...now].find((index) => !held.has(index));
      if (fresh !== undefined) {
        finish(bindPadButton(bindings(), target.controlId, fresh));
        return;
      }
      held = new Set([...held].filter((index) => now.has(index)));
      padFrame = requestAnimationFrame(poll);
    };
    padFrame = requestAnimationFrame(poll);
  }

  const keysOf = (control: Control): string => {
    const codes = bindings().keyboard[control.id] ?? [];
    return codes.length ? codes.map(keyLabel).join(" / ") : "—";
  };
  const padOf = (control: Control): string => {
    const indices = bindings().gamepad[control.id] ?? [];
    return indices.length ? indices.map(padButtonLabel).join(" / ") : "—";
  };
  const isListening = (control: Control, device: "keyboard" | "gamepad"): boolean => {
    const target = listening();
    return target !== null && target.controlId === control.id && target.device === device;
  };

  return (
    <details class="voland-controls" data-testid="controls">
      <summary>Controls (keyboard and gamepad; click any key to change it)</summary>
      <p class="voland-controls-hint" data-testid="controls-hint">
        Click a key or gamepad button below, then press the one you want. Esc cancels, Backspace
        removes it. A key moves off any other control it was on. Changes apply at once, even mid-game,
        and are kept in this browser. Game prompts show Switch buttons; this list maps them to yours.
      </p>
      <div class="voland-bindings" role="table">
        <div class="voland-bindings-head" role="row">
          <span role="columnheader">Switch</span>
          <span role="columnheader">Keyboard</span>
          <span role="columnheader">Gamepad</span>
        </div>
        <For each={CONTROLS}>
          {(control) => (
            <div class="voland-bindings-row" role="row" data-control={control.id}>
              <span role="cell">{control.label}</span>
              <button
                type="button"
                role="cell"
                data-testid={`bind-key-${control.id}`}
                classList={{ listening: isListening(control, "keyboard") }}
                onClick={(event) => {
                  event.currentTarget.blur(); /* Space/Enter must reach the capture, not re-click */
                  setListening(isListening(control, "keyboard") ? null : { controlId: control.id, device: "keyboard" });
                }}
              >
                {isListening(control, "keyboard") ? "press a key…" : keysOf(control)}
              </button>
              <Show when={isPadBindable(control)} fallback={<span role="cell" class="voland-bindings-fixed">gamepad stick</span>}>
                <button
                  type="button"
                  role="cell"
                  data-testid={`bind-pad-${control.id}`}
                  classList={{ listening: isListening(control, "gamepad") }}
                  onClick={(event) => {
                    event.currentTarget.blur();
                    if (isListening(control, "gamepad")) setListening(null);
                    else listenForPad(control.id);
                  }}
                >
                  {isListening(control, "gamepad") ? "press a gamepad button…" : padOf(control)}
                </button>
              </Show>
            </div>
          )}
        </For>
      </div>
      <div class="voland-player-bar">
        <button type="button" data-testid="bindings-reset" onClick={() => finish(DEFAULT_BINDINGS)}>
          Reset to defaults
        </button>
      </div>
    </details>
  );
}

export default ControlsLegend;
