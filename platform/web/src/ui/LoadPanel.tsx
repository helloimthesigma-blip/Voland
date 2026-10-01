/**
 * Game-file picker and load outcome. Phase 1 scope: pick one decrypted
 * Program NCA, have the core load it through vmm (§12), and report the
 * result - including §1.6's encrypted-input error, which points at the
 * dumping guide. Uses <input type="file"> rather than the FSA pickers
 * because it is the path every browser has (§15); handle persistence
 * belongs to the Phase 6 library.
 */
import { Match, Switch, createSignal } from "solid-js";
import type { GameLoadOutcome, LoadFailure } from "@bindings/load";
import { describeLoadFailure } from "./load-failure-copy";

interface LoadPanelProps {
  readonly loadGame: (file: File) => Promise<GameLoadOutcome>;
}

type LoadState =
  | { readonly kind: "idle" }
  | { readonly kind: "loading"; readonly fileName: string }
  | { readonly kind: "loaded";  readonly fileName: string; readonly titleId: string; readonly entryPoint: bigint }
  | { readonly kind: "failed";  readonly fileName: string; readonly failure: LoadFailure };

function LoadPanel(props: LoadPanelProps) {
  const [state, setState] = createSignal<LoadState>({ kind: "idle" });
  let input: HTMLInputElement | undefined;

  async function onFileChosen(event: Event & { currentTarget: HTMLInputElement }): Promise<void> {
    const file = event.currentTarget.files?.[0];
    event.currentTarget.value = ""; // picking the same file again re-triggers change
    if (!file) return;
    setState({ kind: "loading", fileName: file.name });
    const outcome = await props.loadGame(file);
    setState(outcome.success
      ? { kind: "loaded", fileName: file.name, titleId: outcome.titleId, entryPoint: outcome.entryPoint }
      : { kind: "failed", fileName: file.name, failure: outcome.failure });
  }

  return (
    <div class="voland-load" data-testid="load-panel">
      <input
        ref={input}
        type="file"
        class="voland-load-input"
        data-testid="load-input"
        onChange={(event) => { void onFileChosen(event); }}
      />
      <button
        type="button"
        class="voland-load-button"
        disabled={state().kind === "loading"}
        onClick={() => input?.click()}
      >
        Load decrypted NCA…
      </button>

      <Switch>
        <Match when={(() => { const s = state(); return s.kind === "loading" ? s : null; })()}>
          {(loading) => <p class="voland-load-note">Loading {loading().fileName}…</p>}
        </Match>
        <Match when={(() => { const s = state(); return s.kind === "loaded" ? s : null; })()}>
          {(loaded) => (
            <div class="voland-load-result" data-testid="load-success">
              <h3>Loaded {loaded().fileName}</h3>
              <p>
                Title <code>{loaded().titleId}</code>, entry point{" "}
                <code>0x{loaded().entryPoint.toString(16)}</code>. Its code is mapped into
                guest memory, but nothing runs yet: executing guest code needs the
                interpreter, which arrives in Phase 2.
              </p>
            </div>
          )}
        </Match>
        <Match when={(() => { const s = state(); return s.kind === "failed" ? s : null; })()}>
          {(failed) => {
            const copy = () => describeLoadFailure(failed().failure);
            return (
              <div class="voland-load-result voland-load-error" role="alert" data-testid="load-error"
                   data-reason={failed().failure.reason}>
                <h3>{copy().heading}</h3>
                <p>{copy().explanation}</p>
                {copy().guideUrl !== null && (
                  <p>
                    <a href={copy().guideUrl ?? ""} target="_blank" rel="noopener noreferrer"
                       data-testid="dumping-guide-link">
                      Read the dumping guide
                    </a>
                  </p>
                )}
                <p class="voland-load-detail">{failed().fileName}: {failed().failure.message}</p>
              </div>
            );
          }}
        </Match>
      </Switch>
    </div>
  );
}

export default LoadPanel;
