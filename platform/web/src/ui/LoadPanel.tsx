/**
 * Load a decrypted Program NCA or a homebrew NRO, then show what the guest
 * prints while it runs. Uses <input type="file"> - the path every browser
 * has (§15); handle persistence belongs to the Phase 6 library. Encrypted
 * input gets §1.6's dumping-guide error. "Run the demo" loads Voland's own
 * hand-written homebrew (public/demo/hello.nro, from tests/guest/hello.s).
 */
import { For, Match, Show, Switch, createSignal, onCleanup, onMount } from "solid-js";
import type { GameLoadOutcome, LoadFailure } from "@bindings/load";
import { type GuestConsoleState, getGuestConsole, subscribeGuestConsole } from "../guest-console";
import { describeLoadFailure } from "./load-failure-copy";

interface LoadPanelProps {
  readonly loadGame: (file: File) => Promise<GameLoadOutcome>;
}

type LoadState =
  | { readonly kind: "idle" }
  | { readonly kind: "loading"; readonly fileName: string }
  | { readonly kind: "loaded";  readonly fileName: string; readonly titleId: string; readonly entryPoint: bigint }
  | { readonly kind: "failed";  readonly fileName: string; readonly failure: LoadFailure };

const DEMO_URL = "/demo/hello.nro";

const RUN_STATE_LABELS: Readonly<Record<GuestConsoleState["runState"], string>> = {
  idle: "loaded",
  running: "running",
  paused: "paused",
  exited: "finished",
  crashed: "crashed",
  deadlock: "stuck (every thread is waiting)",
};

function LoadPanel(props: LoadPanelProps) {
  const [state, setState] = createSignal<LoadState>({ kind: "idle" });
  const [guest, setGuest] = createSignal<GuestConsoleState>(getGuestConsole());
  let input: HTMLInputElement | undefined;

  onMount(() => {
    const off = subscribeGuestConsole(setGuest);
    onCleanup(off);
  });

  async function load(file: File): Promise<void> {
    setState({ kind: "loading", fileName: file.name });
    const outcome = await props.loadGame(file);
    setState(outcome.success
      ? { kind: "loaded", fileName: file.name, titleId: outcome.titleId, entryPoint: outcome.entryPoint }
      : { kind: "failed", fileName: file.name, failure: outcome.failure });
  }

  async function onFileChosen(event: Event & { currentTarget: HTMLInputElement }): Promise<void> {
    const file = event.currentTarget.files?.[0];
    event.currentTarget.value = ""; // picking the same file again re-triggers change
    if (file) await load(file);
  }

  async function runDemo(): Promise<void> {
    const response = await fetch(DEMO_URL);
    const bytes = await response.arrayBuffer();
    await load(new File([bytes], "hello.nro", { type: "application/octet-stream" }));
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
      <div class="voland-load-actions">
        <button
          type="button"
          class="voland-load-button"
          disabled={state().kind === "loading"}
          onClick={() => input?.click()}
        >
          Load NCA or homebrew NRO…
        </button>
        <button
          type="button"
          class="voland-load-button voland-load-secondary"
          data-testid="run-demo"
          disabled={state().kind === "loading"}
          onClick={() => { void runDemo(); }}
        >
          Run the demo
        </button>
      </div>

      <Switch>
        <Match when={(() => { const s = state(); return s.kind === "loading" ? s : null; })()}>
          {(loading) => <p class="voland-load-note">Loading {loading().fileName}…</p>}
        </Match>
        <Match when={(() => { const s = state(); return s.kind === "loaded" ? s : null; })()}>
          {(loaded) => (
            <div class="voland-load-result" data-testid="load-success">
              <h3>
                {loaded().fileName}
                <span class="voland-run-state" data-testid="run-state" data-state={guest().runState}>
                  {RUN_STATE_LABELS[guest().runState]}
                </span>
              </h3>
              <p>
                Title <code>{loaded().titleId}</code>, entry point{" "}
                <code>0x{loaded().entryPoint.toString(16)}</code>.
              </p>
              <Show when={guest().lines.length > 0}>
                <pre class="voland-guest-console" data-testid="guest-console">
                  <For each={guest().lines}>{(line) => <>{line}{"\n"}</>}</For>
                </pre>
              </Show>
              <Show when={guest().runState === "crashed" || guest().runState === "deadlock"}>
                <p class="voland-load-detail">{guest().detail}</p>
              </Show>
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
