/**
 * Load a decrypted Program NCA or a homebrew NRO, then show what the guest
 * prints while it runs. Uses <input type="file"> - the path every browser
 * has (§15); handle persistence belongs to the Phase 6 library. Encrypted
 * input gets §1.6's dumping-guide error. "Run the demo" loads Voland's own
 * hand-written homebrew (public/demo/hello.nro, from tests/guest/hello.s).
 */
import { For, Match, Show, Switch, createSignal, onCleanup, onMount } from "solid-js";
import type { GameLoadOutcome, LoadFailure, SdImportOutcome } from "@bindings/load";
import { type GuestConsoleState, getGuestConsole, subscribeGuestConsole } from "../guest-console";
import { describeLoadFailure } from "./load-failure-copy";
import GameLibrary from "./GameLibrary";
import SaveStates from "./SaveStates";
import CompatReport from "./CompatReport";
import {
  type LibraryGame, canKeepFiles, forgetGame, listGames, markPlayed, openGameFile, pickGameFile, rememberGame,
} from "../library";

interface LoadPanelProps {
  readonly loadGame: (file: File) => Promise<GameLoadOutcome>;
  readonly addToSdCard: (files: readonly File[]) => Promise<SdImportOutcome>;
  readonly clearSdCard: () => Promise<SdImportOutcome>;
  readonly setPaused: (paused: boolean) => void;
  readonly gpuAdapter: string;
}

type LoadState =
  | { readonly kind: "idle" }
  | { readonly kind: "loading"; readonly fileName: string }
  | { readonly kind: "loaded";  readonly fileName: string; readonly titleId: string; readonly entryPoint: bigint }
  | { readonly kind: "failed";  readonly fileName: string; readonly failure: LoadFailure };

const DEMO_URL = `${import.meta.env.BASE_URL}demo/hello.nro`;
/* How long the "starting up" hint stays after a game loads. */
const BOOT_HINT_MS = 10 * 60 * 1000;

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
  let sdInput: HTMLInputElement | undefined;
  const [sd, setSd] = createSignal<SdImportOutcome | null>(null);
  const [bootHint, setBootHint] = createSignal(false);
  const [games, setGames] = createSignal<readonly LibraryGame[]>([]);
  const refreshLibrary = async (): Promise<void> => { setGames(await listGames()); };
  let bootHintTimer = 0;
  onCleanup(() => window.clearTimeout(bootHintTimer));

  async function onSdFilesChosen(event: Event & { currentTarget: HTMLInputElement }): Promise<void> {
    const files = Array.from(event.currentTarget.files ?? []);
    event.currentTarget.value = "";
    if (files.length > 0) setSd(await props.addToSdCard(files));
  }

  onMount(() => {
    const off = subscribeGuestConsole(setGuest);
    onCleanup(off);
    void refreshLibrary();
  });

  /* The game canvas itself goes fullscreen: it lives in <body>, pinned over
   * the screen box (mount.tsx), so the box element has nothing to show. */
  function enterFullscreen(): void {
    const canvas = document.getElementById("game");
    void canvas?.requestFullscreen?.().catch(() => undefined);
  }

  async function load(file: File, gameId: string | null = null): Promise<void> {
    setState({ kind: "loading", fileName: file.name });
    const outcome = await props.loadGame(file);
    if (outcome.success && gameId) {
      await markPlayed(gameId, outcome.titleId);
      await refreshLibrary();
    }
    setState(outcome.success
      ? { kind: "loaded", fileName: file.name, titleId: outcome.titleId, entryPoint: outcome.entryPoint }
      : { kind: "failed", fileName: file.name, failure: outcome.failure });
    window.clearTimeout(bootHintTimer);
    setBootHint(outcome.success);
    if (outcome.success) bootHintTimer = window.setTimeout(() => setBootHint(false), BOOT_HINT_MS);
  }

  async function onFileChosen(event: Event & { currentTarget: HTMLInputElement }): Promise<void> {
    const file = event.currentTarget.files?.[0];
    event.currentTarget.value = ""; // picking the same file again re-triggers change
    if (!file) return;
    const game = await rememberGame(file, null);
    await refreshLibrary();
    await load(file, game.id);
  }

  /* Picks a game: through the File System Access picker where the browser
   * has one (so the library can keep the file), else a plain file input. */
  async function chooseGame(): Promise<void> {
    if (!canKeepFiles()) {
      input?.click();
      return;
    }
    const picked = await pickGameFile();
    if (!picked) return;
    const game = await rememberGame(picked.file, picked.handle);
    await refreshLibrary();
    await load(picked.file, game.id);
  }

  async function launch(game: LibraryGame): Promise<void> {
    const file = await openGameFile(game);
    if (file) {
      await load(file, game.id);
      return;
    }
    /* No handle, permission refused, or the file changed: pick it again
     * (it is remembered under the same entry if it is the same file). */
    if (canKeepFiles()) await chooseGame();
    else input?.click();
  }

  async function addDropped(items: DataTransferItemList): Promise<void> {
    const pending = Array.from(items).filter((item) => item.kind === "file").map(async (item) => {
      const withHandle = item as DataTransferItem & { getAsFileSystemHandle?: () => Promise<FileSystemHandle | null> };
      const handle = await withHandle.getAsFileSystemHandle?.().catch(() => null) ?? null;
      if (handle && handle.kind === "file") {
        const fileHandle = handle as FileSystemFileHandle;
        await rememberGame(await fileHandle.getFile(), fileHandle);
        return;
      }
      const file = item.getAsFile();
      if (file) await rememberGame(file, null);
    });
    await Promise.all(pending);
    await refreshLibrary();
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
      <input
        ref={sdInput}
        type="file"
        multiple
        class="voland-load-input"
        data-testid="sd-input"
        onChange={(event) => { void onSdFilesChosen(event); }}
      />
      <GameLibrary
        games={games()}
        busy={state().kind === "loading"}
        onLaunch={(game) => { void launch(game); }}
        onForget={(game) => { void forgetGame(game.id).then(refreshLibrary); }}
        onDropFiles={(items) => { void addDropped(items); }}
      />
      <div class="voland-load-actions">
        <button
          type="button"
          class="voland-load-button"
          disabled={state().kind === "loading"}
          data-testid="load-game"
          onClick={() => { void chooseGame(); }}
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
        <button
          type="button"
          class="voland-load-button voland-load-secondary"
          data-testid="sd-add"
          onClick={() => sdInput?.click()}
        >
          Add homebrew to SD card…
        </button>
        <button
          type="button"
          class="voland-load-button voland-load-secondary"
          data-testid="sd-clear"
          onClick={() => { void props.clearSdCard().then(() => setSd({ added: [], failed: [] })); }}
        >
          Empty SD card
        </button>
      </div>
      <Show when={sd()}>
        {(result) => (
          <p class="voland-load-note" data-testid="sd-result">
            {result().added.length === 0 && result().failed.length === 0
              ? "SD card emptied."
              : `SD card: added ${result().added.length} file(s)` +
                (result().added.length > 0 ? ` (${result().added.join(", ")})` : "") +
                (result().failed.length > 0 ? `; could not add ${result().failed.join(", ")}` : "") + "."}
          </p>
        )}
      </Show>

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
                <Show when={guest().runState === "running" && guest().fps > 0}>
                  <span class="voland-run-state" data-testid="fps">{guest().fps} fps</span>
                </Show>
              </h3>
              <div class="voland-player-bar">
                <Show when={guest().runState === "running" || guest().runState === "paused"}>
                  <button
                    type="button"
                    data-testid="pause"
                    onClick={() => props.setPaused(guest().runState === "running")}
                  >
                    {guest().runState === "paused" ? "Resume" : "Pause"}
                  </button>
                </Show>
                <button type="button" data-testid="fullscreen" onClick={enterFullscreen}>
                  Fullscreen
                </button>
              </div>
              <Show when={guest().runState !== "idle"}>
                <SaveStates titleId={loaded().titleId} />
              </Show>
              <CompatReport titleId={loaded().titleId} fileName={loaded().fileName} gpuAdapter={props.gpuAdapter} />
              <p>
                Title <code>{loaded().titleId}</code>, entry point{" "}
                <code>0x{loaded().entryPoint.toString(16)}</code>.
              </p>
              <Show when={guest().lines.length > 0}>
                <pre class="voland-guest-console" data-testid="guest-console">
                  <For each={guest().lines}>{(line) => <>{line}{"\n"}</>}</For>
                </pre>
              </Show>
              <Show when={bootHint() && guest().runState === "running"}>
                <p class="voland-load-note" data-testid="boot-hint">
                  Starting up: big games run well below full speed here, so the first
                  logos and the title screen can take several minutes. The frame counter
                  shows it is working; sound is slowed down to match.
                </p>
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
