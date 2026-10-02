/**
 * The software keyboard: when a title opens it, the player types here.
 * OK sends the text (trimmed to the title's maximum length); Cancel or
 * Escape tells the title the keyboard was closed.
 */
import { Show, createSignal, onCleanup, onMount } from "solid-js";
import type { TextInputRequest } from "@bindings/protocol";
import { getTextInput, subscribeTextInput } from "../text-input-store";

interface TextInputDialogProps {
  readonly respond: (text: string, accepted: boolean) => void;
}

function TextInputDialog(props: TextInputDialogProps) {
  const [request, setRequest] = createSignal<TextInputRequest | null>(getTextInput());
  const [text, setText] = createSignal("");
  let field: HTMLInputElement | undefined;

  onMount(() => {
    const off = subscribeTextInput((next) => {
      setRequest(next);
      setText(next?.initial ?? "");
      if (next) queueMicrotask(() => field?.focus());
    });
    onCleanup(off);
  });

  const tooShort = (): boolean => {
    const r = request();
    return r !== null && r.minLength > 0 && [...text()].length < r.minLength;
  };

  function submit(event?: Event): void {
    event?.preventDefault();
    const r = request();
    if (!r || tooShort()) return;
    const chars = [...text()];
    props.respond(r.maxLength > 0 ? chars.slice(0, r.maxLength).join("") : text(), true);
  }

  return (
    <Show when={request()}>
      {(r) => (
        <div class="voland-dialog-backdrop" data-testid="text-input">
          <form class="voland-dialog" onSubmit={submit}>
            <h3>{r().header || "Enter text"}</h3>
            <Show when={r().sub}>
              <p class="voland-dialog-sub">{r().sub}</p>
            </Show>
            <input
              ref={field}
              data-testid="text-input-field"
              type={r().password ? "password" : "text"}
              value={text()}
              placeholder={r().guide}
              maxLength={r().maxLength > 0 ? r().maxLength : undefined}
              onInput={(e) => setText(e.currentTarget.value)}
              onKeyDown={(e) => {
                if (e.key === "Escape") props.respond("", false);
              }}
            />
            <div class="voland-dialog-buttons">
              <button type="button" data-testid="text-input-cancel" onClick={() => props.respond("", false)}>
                Cancel
              </button>
              <button type="submit" data-testid="text-input-ok" disabled={tooShort()}>
                OK
              </button>
            </div>
          </form>
        </div>
      )}
    </Show>
  );
}

export default TextInputDialog;
