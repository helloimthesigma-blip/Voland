/**
 * platform/web/src/ui/App.tsx
 *
 * SolidJS shell mounted once the boot sequence reports all workers ready.
 * One column: the screen, the running game's controls under it, then one
 * row of tabs - Games (library, load buttons, Voland folder), Saves, Mods,
 * Controls, Settings (with the rarely needed SD card, system files, event
 * log and machine facts under Advanced). Only the chosen tab shows; the
 * others stay mounted so their state survives switching. A visit starts
 * on Games.
 */
import { For, Show, createSignal, onCleanup, onMount } from "solid-js";
import type { JSX } from "solid-js";
import type { GameLoadOutcome, SdImportOutcome, SystemFilesApi } from "@bindings/load";
import type { LogEntry } from "../log";
import { getLogHistory, getStatus, subscribeLogs, subscribeStatus } from "../log";
import SavesPanel from "./SavesPanel";
import ModsPanel from "./ModsPanel";
import ControlsLegend from "./ControlsLegend";
import TextInputDialog from "./TextInputDialog";
import LoadPanel from "./LoadPanel";
import FrameSkipSetting from "./FrameSkipSetting";
import ThreadsSetting from "./ThreadsSetting";
import AutoStateSetting from "./AutoStateSetting";
import { inLauncherFrame, openInBlankTab } from "../blank-launch";

interface AppProps {
  readonly adapterLabel: string;
  readonly cpuBackend:   string;
  readonly guestRamMiB:  number;
  readonly loadGame:     (file: File, extras?: readonly File[]) => Promise<GameLoadOutcome>;
  readonly addToSdCard:  (files: readonly File[]) => Promise<SdImportOutcome>;
  readonly clearSdCard:  () => Promise<SdImportOutcome>;
  readonly systemFiles:  SystemFilesApi;
  readonly setPaused:    (paused: boolean) => void;
  readonly setFrameSkip: (frames: number) => void;
  readonly setHostCores: (cores: number) => void;
  readonly respondText:  (text: string, accepted: boolean) => void;
}

const PAGE_STARTED = Date.now();

const TABS = [
  { id: "games", label: "Games" },
  { id: "saves", label: "Saves" },
  { id: "mods", label: "Mods" },
  { id: "controls", label: "Controls" },
  { id: "settings", label: "Settings" },
] as const;
type TabId = (typeof TABS)[number]["id"];

/** "[12:34] [WARN ] message": minutes:seconds since the page opened. */
function formatLine(entry: LogEntry): string {
  const s = Math.max(0, Math.floor((entry.timestamp - PAGE_STARTED) / 1000));
  const stamp = `${Math.floor(s / 60)}:${String(s % 60).padStart(2, "0")}`;
  return `[${stamp}] [${entry.level.toUpperCase().padEnd(5, " ")}] ${entry.message}`;
}

function App(props: AppProps) {
  const [logs, setLogs] = createSignal<readonly LogEntry[]>(getLogHistory().slice());
  const [status, setStatusSig] = createSignal(getStatus());

  onMount(() => {
    const offLogs = subscribeLogs((entry) => {
      setLogs((prev) => {
        const next = prev.length >= 500 ? prev.slice(1) : prev.slice();
        next.push(entry);
        return next;
      });
    });
    const offStatus = subscribeStatus((text) => setStatusSig(text));
    onCleanup(() => { offLogs(); offStatus(); });
  });

  const [libraryMount, setLibraryMount] = createSignal<HTMLElement>();
  const [advancedMount, setAdvancedMount] = createSignal<HTMLElement>();
  const [problemsOnly, setProblemsOnly] = createSignal(false);
  const [blankNote, setBlankNote] = createSignal<string | null>(null);
  const [tab, setTab] = createSignal<TabId>("games");
  const pane = (id: TabId, body: JSX.Element): JSX.Element => (
    <div class="voland-tab-pane" role="tabpanel" id={`voland-tab-${id}`} hidden={tab() !== id} data-testid={`tab-pane-${id}`}>
      {body}
    </div>
  );

  return (
    <div class="voland-shell">
      <header class="voland-header">
        <div class="voland-brand">
          <span class="voland-title">Voland</span>
          <span class="voland-subtitle">Switch emulator in your browser</span>
        </div>
        <span class="voland-status" data-testid="status">{status()}</span>
      </header>

      <section class="voland-main">
        <div class="voland-hero">
          <div class="voland-screen" data-voland-screen data-testid="screen" />
          <LoadPanel loadGame={props.loadGame} addToSdCard={props.addToSdCard} clearSdCard={props.clearSdCard}
                     systemFiles={props.systemFiles} setPaused={props.setPaused} gpuAdapter={props.adapterLabel}
                     libraryMount={libraryMount} advancedMount={advancedMount} />
          <TextInputDialog respond={props.respondText} />

          <nav class="voland-tabs" role="tablist">
            <For each={TABS}>
              {(t) => (
                <button type="button" role="tab" aria-selected={tab() === t.id} aria-controls={`voland-tab-${t.id}`}
                        classList={{ active: tab() === t.id }} data-testid={`tab-${t.id}`} onClick={() => setTab(t.id)}>
                  {t.label}
                </button>
              )}
            </For>
          </nav>

          {pane("games", <div class="voland-tab-stack" ref={setLibraryMount} />)}
          {pane("saves", <section class="voland-panel"><SavesPanel shown={() => tab() === "saves"} /></section>)}
          {pane("mods", <section class="voland-panel"><ModsPanel /></section>)}
          {pane("controls", <section class="voland-panel"><ControlsLegend /></section>)}
          {pane("settings", (
            <div class="voland-tab-stack">
              <section class="voland-panel">
                <FrameSkipSetting setFrameSkip={props.setFrameSkip} />
                <ThreadsSetting setHostCores={props.setHostCores} />
                <AutoStateSetting />
              </section>
              <details class="voland-panel voland-advanced" data-testid="advanced">
                <summary>Advanced: homebrew SD card, system files, event log, this machine</summary>
                <div class="voland-tab-stack">
                  <div class="voland-tab-stack" ref={setAdvancedMount} />
                  <section class="voland-panel">
                    <h4>This machine</h4>
                    <dl class="voland-facts">
                      <div><dt>CPU</dt><dd>{props.cpuBackend}</dd></div>
                      <div><dt>GPU</dt><dd>{props.adapterLabel}</dd></div>
                      <div><dt>RAM</dt><dd>{props.guestRamMiB} MiB</dd></div>
                    </dl>
                    <Show when={!inLauncherFrame()}>
                      <div class="voland-setting">
                        <button type="button" class="voland-load-button voland-load-secondary" data-testid="open-blank"
                                onClick={() => { if (!openInBlankTab()) setBlankNote("The browser blocked the new tab; allow pop-ups for this site."); }}>
                          Open in about:blank
                        </button>
                        <small>Moves Voland into a blank tab (this one closes its copy).</small>
                      </div>
                      <Show when={blankNote()}>{(note) => <p class="voland-load-note">{note()}</p>}</Show>
                    </Show>
                  </section>
                  <section class="voland-panel voland-log">
                    <h4>Event log</h4>
                    <div class="voland-log-tools">
                      <label>
                        <input type="checkbox" checked={problemsOnly()} onChange={(e) => setProblemsOnly(e.currentTarget.checked)} />
                        Problems only
                      </label>
                      <button type="button" class="voland-load-button voland-load-secondary" data-testid="log-copy"
                              onClick={() => void navigator.clipboard?.writeText(logs().map(formatLine).join("\n"))}>
                        Copy log
                      </button>
                    </div>
                    <div class="voland-log-scroll">
                      <For each={logs().filter((e) => problemsOnly() ? e.level === "warn" || e.level === "error" : e.level !== "debug" && e.level !== "trace")}>
                        {(entry) => <p class={`log-line log-${entry.level}`}>{formatLine(entry)}</p>}
                      </For>
                    </div>
                  </section>
                </div>
              </details>
            </div>
          ))}
        </div>
      </section>
    </div>
  );
}

export default App;
