/**
 * Load a decrypted Program NCA or a homebrew NRO, then show what the guest
 * prints while it runs. Uses <input type="file"> - the path every browser
 * has (§15); handle persistence belongs to the Phase 6 library. Encrypted
 * input gets §1.6's dumping-guide error. "Run the demo" loads Voland's own
 * hand-written homebrew (public/demo/hello.nro, from tests/guest/hello.s).
 */
import { For, Match, Show, Switch, createEffect, createSignal, onCleanup, onMount } from "solid-js";
import { registerRestart } from "../restart";
import { Portal } from "solid-js/web";
import { SYSTEM_FILES, systemFileName } from "@bindings/load";
import type { GameLoadOutcome, LoadFailure, SdImportOutcome, SystemFilesApi } from "@bindings/load";
import { type GuestConsoleState, getGuestConsole, subscribeGuestConsole } from "../guest-console";
import { describeLoadFailure } from "./load-failure-copy";
import GameLibrary from "./GameLibrary";
import SaveStates from "./SaveStates";
import CompatReport, { buildReportText } from "./CompatReport";
import type { SavestateInfo } from "@bindings/protocol";
import { latestState, listStates, loadState } from "../savestates";
import { subscribeAutoStates } from "../autostate";
import { memoryUseOf, memoryWarning } from "../memory-guard";
import { setCurrentGame } from "../game-settings";
import { titleNames } from "../saves";
import FilesPanel from "./FilesPanel";
import GettingStarted from "./GettingStarted";
import {
  type LibraryGame, canKeepFiles, canKeepFolders, forgetGame, listGames, markPlayed, openGameFile, pickGameFile,
  pickGameFolder, rememberGame,
} from "../library";
import { type GameFolder, folderFiles, noGameMessage, scanGameFolder } from "../game-folder";
import { type FolderScan, freshFiles } from "../voland-folder";

interface LoadPanelProps {
  readonly loadGame: (file: File, extras?: readonly File[]) => Promise<GameLoadOutcome>;
  readonly addToSdCard: (files: readonly File[]) => Promise<SdImportOutcome>;
  readonly clearSdCard: () => Promise<SdImportOutcome>;
  readonly systemFiles: SystemFilesApi;
  readonly setPaused: (paused: boolean) => void;
  readonly gpuAdapter: string;
  /** Where the SD card and system-file tools go (the sidebar). */
  /** Where the library, the load buttons and the Voland folder go (the Games tab). */
  readonly libraryMount: () => HTMLElement | undefined;
  /** Where the SD card and system files go (Settings, under Advanced). */
  readonly advancedMount: () => HTMLElement | undefined;
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
  let folderInput: HTMLInputElement | undefined;
  let sdInput: HTMLInputElement | undefined;
  let systemInput: HTMLInputElement | undefined;
  let firmwareInput: HTMLInputElement | undefined;
  const [systemIds, setSystemIds] = createSignal<readonly string[]>([]);
  const [systemNote, setSystemNote] = createSignal<string | null>(null);
  /* Which archive a bare RomFS dump is (an NCA says so itself). */
  const [systemKind, setSystemKind] = createSignal<string>(SYSTEM_FILES[0]?.id ?? "");
  const refreshSystemFiles = async (): Promise<void> => { setSystemIds(await props.systemFiles.list()); };

  async function onSystemFilesChosen(event: Event & { currentTarget: HTMLInputElement }): Promise<void> {
    const files = Array.from(event.currentTarget.files ?? []);
    event.currentTarget.value = "";
    if (files.length === 0) return;
    setSystemNote(files.length === 1 ? `Adding ${files[0]?.name ?? "the file"}…` : `Scanning ${files.length} files…`);
    const outcome = await props.systemFiles.add(files, systemKind());
    const parts: string[] = [];
    if (outcome.added.length > 0) parts.push(`Added ${outcome.added.map((id) => `${systemFileName(id)} (${id})`).join(", ")}.`);
    else parts.push("Nothing added.");
    if (files.length > 1) parts.push(`${outcome.skipped} file(s) skipped: not ones games need.`);
    if (outcome.errors.length > 0) parts.push(outcome.errors.join("; ") + ".");
    setSystemNote(parts.join(" "));
    await refreshSystemFiles();
  }

  const [sd, setSd] = createSignal<SdImportOutcome | null>(null);
  const [bootHint, setBootHint] = createSignal(false);
  const [games, setGames] = createSignal<readonly LibraryGame[]>([]);
  /* The newest save state per title: "Continue" on its tile. */
  const [resumable, setResumable] = createSignal<ReadonlyMap<string, SavestateInfo>>(new Map());
  const refreshResumable = async (): Promise<void> => {
    const newest = new Map<string, SavestateInfo>();
    for (const state of await listStates()) if (!newest.has(state.titleId)) newest.set(state.titleId, state);
    setResumable(newest);
  };
  const refreshLibrary = async (): Promise<void> => {
    setGames(await listGames());
    await refreshResumable();
  };
  /* "Heavy game" before and while one loads (src/memory-guard.ts). */
  const [memoryNote, setMemoryNote] = createSignal<string | null>(null);
  /* A "Continue" waiting for its game to load. */
  let pendingContinue: SavestateInfo | null = null;
  /* The Voland folder's last scan (FilesPanel): its games launch from it. */
  let folderScan: FolderScan | null = null;

  async function onFolderScan(scan: FolderScan | null, root: FileSystemDirectoryHandle | null): Promise<void> {
    folderScan = scan;
    if (!scan) return;
    for (const game of scan.games) {
      if (game.dir !== null) {
        const dir = root ? await root.getDirectoryHandle("games").then((g) => g.getDirectoryHandle(game.dir ?? "")).catch(() => null) : null;
        await rememberGame(game.program.file, null, { name: game.dir, handle: dir });
      } else {
        await rememberGame(game.program.file, game.program.handle ?? null);
      }
    }
    await refreshLibrary();
  }
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
    onCleanup(subscribeAutoStates(() => { void refreshResumable(); }));
    void refreshLibrary();
    void refreshSystemFiles();
  });

  /* The game canvas itself goes fullscreen: it lives in <body>, pinned over
   * the screen box (mount.tsx), so the box element has nothing to show. */
  function enterFullscreen(): void {
    const canvas = document.getElementById("game");
    void canvas?.requestFullscreen?.().catch(() => undefined);
  }

  async function load(file: File, gameId: string | null = null, extras: readonly File[] = []): Promise<boolean> {
    const titleId = games().find((g) => g.id === gameId)?.titleId ?? null;
    const deviceMemory = (navigator as Navigator & { readonly deviceMemory?: number }).deviceMemory ?? null;
    setMemoryNote(memoryWarning(titleId ? memoryUseOf(titleId) : null, file.size, deviceMemory));
    setState({ kind: "loading", fileName: file.name });
    const outcome = await props.loadGame(file, extras);
    /* The game's own settings (frame skip, threads) apply from here. */
    setCurrentGame(outcome.success
      ? { titleId: outcome.titleId, name: outcome.title ?? titleNames()[outcome.titleId] ?? file.name.replace(/\.(nca|nro)$/i, "") }
      : null);
    if (outcome.success && gameId) {
      await markPlayed(gameId, outcome.titleId, outcome.title, outcome.icon);
      await refreshLibrary();
    }
    const relaunch = outcome.success && gameId ? games().find((g) => g.id === gameId) ?? null : null;
    registerRestart(relaunch && outcome.success ? { titleId: outcome.titleId, restart: () => launch(relaunch) } : null);
    setState(outcome.success
      ? { kind: "loaded", fileName: file.name, titleId: outcome.titleId, entryPoint: outcome.entryPoint }
      : { kind: "failed", fileName: file.name, failure: outcome.failure });
    window.clearTimeout(bootHintTimer);
    setBootHint(outcome.success);
    if (outcome.success) bootHintTimer = window.setTimeout(() => setBootHint(false), BOOT_HINT_MS);
    const resume = pendingContinue;
    pendingContinue = null;
    if (outcome.success && resume && resume.titleId === outcome.titleId) await applyContinue(resume);
    return outcome.success;
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

  /* A scanned game folder: remember it and play its Program NCA with the
   * folder's other NCAs (control, update, DLC) beside it. */
  async function loadFolder(folder: GameFolder, name: string, handle: FileSystemDirectoryHandle | null): Promise<void> {
    if (!folder.program) {
      setState({ kind: "failed", fileName: name, failure: {
        reason: folder.encrypted > 0 ? "encrypted-input" : "unsupported-content", message: noGameMessage(folder) } });
      return;
    }
    const game = await rememberGame(folder.program, null, { name, handle });
    await refreshLibrary();
    await load(folder.program, game.id, folder.extras);
  }

  async function onFolderChosen(event: Event & { currentTarget: HTMLInputElement }): Promise<void> {
    const files = Array.from(event.currentTarget.files ?? []);
    event.currentTarget.value = "";
    if (files.length === 0) return;
    const name = files[0]?.webkitRelativePath.split("/")[0] || "game folder";
    await loadFolder(await scanGameFolder(files), name, null);
  }

  /* Picks a game folder: kept as a handle where the browser can, else a
   * plain folder input (asked for again at each launch). */
  async function chooseFolder(): Promise<void> {
    if (!canKeepFolders()) {
      folderInput?.click();
      return;
    }
    const picked = await pickGameFolder();
    if (picked) await loadFolder(picked.folder, picked.name, picked.handle);
  }

  /** Starts `game`; true once it is loaded (false if it failed or a file
   * picker had to be shown instead). */
  async function launch(game: LibraryGame): Promise<boolean> {
    /* A game from the Voland folder brings its update and DLC from there,
     * loose .nca files included. */
    const inFolder = folderScan?.games.find((g) => g.id === game.id);
    if (inFolder) {
      const fresh = await freshFiles(inFolder).catch(() => null);
      if (fresh) return load(fresh.file, game.id, fresh.extras);
    }
    const opened = await openGameFile(game);
    if (opened) return load(opened.file, game.id, opened.extras);
    /* No handle, permission refused, or the file changed: pick it again
     * (it is remembered under the same entry if it is the same file). */
    if (game.folder !== undefined && game.folder !== null) await chooseFolder();
    else if (game.id.startsWith("folder:")) folderInput?.click();
    else if (canKeepFiles()) await chooseGame();
    else input?.click();
    return false;
  }

  /* Recovery: a message about going back to a save state. */
  const [recoverNote, setRecoverNote] = createSignal<string | null>(null);

  /* "Continue": start the game, then go back to its newest save state.
   * When the browser has to ask for the file first, the state waits for
   * that game's next load (load() applies it). */
  async function continueGame(game: LibraryGame, state: SavestateInfo): Promise<void> {
    pendingContinue = state;
    await launch(game);
  }

  async function applyContinue(saved: SavestateInfo): Promise<void> {
    setRecoverNote("Going back to where you were…");
    const result = await loadState(saved.id);
    setRecoverNote(result.ok ? null : `Could not continue: ${result.error} The game started from the beginning instead.`);
  }

  /* After a crash or a hang: the newest save state of the running game. */
  const [crashState, setCrashState] = createSignal<SavestateInfo | null>(null);
  const [reportCopied, setReportCopied] = createSignal(false);
  createEffect(() => {
    const s = state();
    const stopped = guest().runState === "crashed" || guest().runState === "deadlock";
    if (s.kind !== "loaded" || !stopped) {
      setCrashState(null);
      return;
    }
    setReportCopied(false);
    void latestState(s.titleId).then(setCrashState);
  });

  async function goBack(saved: SavestateInfo): Promise<void> {
    setRecoverNote("Going back…");
    const result = await loadState(saved.id);
    setRecoverNote(result.ok ? null : `Could not go back: ${result.error}`);
  }

  async function copyCrashReport(titleId: string, fileName: string): Promise<void> {
    const text = await buildReportText({ titleId, fileName, gpuAdapter: props.gpuAdapter });
    await navigator.clipboard?.writeText(text).then(() => setReportCopied(true)).catch(() => setReportCopied(false));
  }

  async function addDropped(items: DataTransferItemList): Promise<void> {
    const pending = Array.from(items).filter((item) => item.kind === "file").map(async (item) => {
      const withHandle = item as DataTransferItem & { getAsFileSystemHandle?: () => Promise<FileSystemHandle | null> };
      const handle = await withHandle.getAsFileSystemHandle?.().catch(() => null) ?? null;
      if (handle && handle.kind === "directory") {
        const dir = handle as FileSystemDirectoryHandle;
        const folder = await scanGameFolder(await folderFiles(dir));
        if (folder.program) await rememberGame(folder.program, null, { name: dir.name, handle: dir });
        return;
      }
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
    <div class="voland-load" data-testid="load-status">
      <input
        ref={input}
        type="file"
        class="voland-load-input"
        data-testid="load-input"
        onChange={(event) => { void onFileChosen(event); }}
      />
      <input
        ref={(el) => { folderInput = el; el.setAttribute("webkitdirectory", ""); }}
        type="file"
        multiple
        class="voland-load-input"
        data-testid="load-folder-input"
        onChange={(event) => { void onFolderChosen(event); }}
      />
      <input
        ref={sdInput}
        type="file"
        multiple
        class="voland-load-input"
        data-testid="sd-input"
        onChange={(event) => { void onSdFilesChosen(event); }}
      />
      <input
        ref={systemInput}
        type="file"
        multiple
        accept=".nca,.romfs,.bin"
        class="voland-load-input"
        data-testid="system-input"
        onChange={(event) => { void onSystemFilesChosen(event); }}
      />
      <input
        ref={(el) => { firmwareInput = el; el.setAttribute("webkitdirectory", ""); }}
        type="file"
        multiple
        class="voland-load-input"
        data-testid="firmware-input"
        onChange={(event) => { void onSystemFilesChosen(event); }}
      />
      <Show when={memoryNote()}>
        {(text) => (
          <p class="voland-load-detail voland-memory-note" role="status" data-testid="memory-warning">
            {text()}{" "}
            <button type="button" class="voland-link-button" onClick={() => setMemoryNote(null)}>Dismiss</button>
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
                  <span class="voland-run-state" data-testid="fps"
                        title="Frames the game drew in the last second; below full speed the game also runs in slow motion">
                    {guest().fps} fps{guest().speed < 1 ? ` · ${Math.round(guest().speed * 100)}% speed` : ""}
                  </span>
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
              <Show when={guest().gpuTrouble !== ""}>
                <p class="voland-load-detail" role="alert" data-testid="gpu-trouble">
                  {guest().gpuTrouble === "out-of-memory"
                    ? "Paused: the graphics card ran out of memory. Close other tabs or apps, then press Resume; a save state first is safest."
                    : "Paused: the browser reset the graphics device, so the picture cannot continue. Make a save state, then reload the page."}
                </p>
              </Show>
              <Show when={guest().runState === "crashed" || guest().runState === "deadlock"}>
                <div class="voland-recover" role="alert" data-testid="crash-recover">
                  <p>
                    <strong>{guest().runState === "crashed" ? "The game crashed." : "The game is stuck."}</strong>{" "}
                    <Show when={crashState()} fallback={<>There is no save state of it to go back to; load the game again.</>}>
                      {(saved) => <>You can go back to {saved().auto ? "the automatic save state" : "your save state"} from{" "}
                        {new Date(saved().createdAt).toLocaleTimeString([], { hour: "numeric", minute: "2-digit" })}.</>}
                    </Show>
                  </p>
                  <div class="voland-player-bar">
                    <Show when={crashState()}>
                      {(saved) => (
                        <button type="button" data-testid="crash-go-back" onClick={() => { void goBack(saved()); }}>Go back</button>
                      )}
                    </Show>
                    <button type="button" data-testid="crash-report"
                            onClick={() => { void copyCrashReport(loaded().titleId, loaded().fileName); }}>
                      {reportCopied() ? "Report copied" : "Copy crash report"}
                    </button>
                  </div>
                  <p class="voland-load-detail">{guest().detail}</p>
                </div>
              </Show>
              <Show when={recoverNote()}>{(text) => <p class="voland-load-note" data-testid="recover-note">{text()}</p>}</Show>
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
      <Show when={props.libraryMount()}>
        {(mount) => (
          <Portal mount={mount()}>
           <div class="voland-load" data-testid="load-panel">
      <GettingStarted />
      <GameLibrary
        games={games()}
        busy={state().kind === "loading"}
        onLaunch={(game) => { void launch(game); }}
        resumable={resumable()}
        onContinue={(game, saved) => { void continueGame(game, saved); }}
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
          disabled={state().kind === "loading"}
          data-testid="load-folder"
          onClick={() => { void chooseFolder(); }}
        >
          Load game folder…
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
      <p class="voland-load-note voland-load-hint">
        A decrypted Program NCA or a homebrew NRO, or the folder of your decrypted game's NCAs (it brings the
        game's name and icon, and any update or DLC in it). Games you load once stay in the library above.
      </p>
            <FilesPanel systemFiles={props.systemFiles} systemIds={systemIds} onSystemFilesChanged={refreshSystemFiles}
                        games={games} onScan={onFolderScan} />
           </div>
          </Portal>
        )}
      </Show>
      <Show when={props.advancedMount()}>
        {(mount) => (
          <Portal mount={mount()}>
            <section class="voland-panel" data-testid="sd-card">
              <h4>SD card</h4>
              <p class="voland-load-note">Homebrew you add appears in a homebrew menu such as hbmenu.</p>
              <div class="voland-load-actions">
                <button
                  type="button"
                  class="voland-load-button voland-load-secondary"
                  data-testid="sd-add"
                  onClick={() => sdInput?.click()}
                >
                  Add homebrew…
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
            </section>
            <section class="voland-panel voland-system-files" data-testid="system-files">
              <h4>System files</h4>
              <p class="voland-load-note">
                Some games need files from your own Switch's system that Voland does not include. Add them from
                your console's dump, decrypted with your own tools like your games: single <code>.nca</code> files
                or raw RomFS dumps, or a whole decrypted firmware folder (only what games need is kept).
              </p>
              <ul>
                <For each={SYSTEM_FILES}>
                  {(f) => (
                    <li data-testid={`system-file-${f.id}`}>
                      <strong>{f.name}</strong> <code>{f.id}</code> - {f.neededBy}:{" "}
                      {systemIds().includes(f.id) ? <span class="voland-ok">added</span> : <span class="voland-missing">not added</span>}
                    </li>
                  )}
                </For>
              </ul>
              <div class="voland-load-actions">
                <button
                  type="button"
                  class="voland-load-button voland-load-secondary"
                  data-testid="system-add"
                  onClick={() => systemInput?.click()}
                >
                  Add system files…
                </button>
                <button
                  type="button"
                  class="voland-load-button voland-load-secondary"
                  data-testid="firmware-add"
                  onClick={() => firmwareInput?.click()}
                >
                  Choose firmware folder…
                </button>
              </div>
              <label class="voland-load-note">
                A raw RomFS dump is the{" "}
                <select value={systemKind()} onChange={(e) => setSystemKind(e.currentTarget.value)} data-testid="system-kind">
                  <For each={SYSTEM_FILES}>{(f) => <option value={f.id}>{f.name}</option>}</For>
                </select>
              </label>
              <Show when={systemNote()}>{(note) => <p class="voland-load-note" data-testid="system-result">{note()}</p>}</Show>
            </section>
          </Portal>
        )}
      </Show>
    </div>
  );
}

export default LoadPanel;
