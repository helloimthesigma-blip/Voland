/**
 * platform/web/src/ui/App.tsx
 *
 * SolidJS shell mounted once the boot sequence reports all workers ready.
 * Left: the screen, the running game's controls under it, then the
 * library and the load buttons. Right: everything set up once - settings,
 * SD card and system files (LoadPanel's tools, portalled in), save data,
 * controls - and the event log. The header carries the status and the
 * backend/GPU/RAM facts.
 */
import { For, createSignal, onCleanup, onMount } from "solid-js";
import type { GameLoadOutcome, SdImportOutcome, SystemFilesApi } from "@bindings/load";
import type { LogEntry } from "../log";
import { getLogHistory, getStatus, subscribeLogs, subscribeStatus } from "../log";
import SavesPanel from "./SavesPanel";
import ControlsLegend from "./ControlsLegend";
import TextInputDialog from "./TextInputDialog";
import LoadPanel from "./LoadPanel";
import FrameSkipSetting from "./FrameSkipSetting";
import ThreadsSetting from "./ThreadsSetting";

interface AppProps {
  readonly adapterLabel: string;
  readonly cpuBackend:   string;
  readonly guestRamMiB:  number;
  readonly loadGame:     (file: File) => Promise<GameLoadOutcome>;
  readonly addToSdCard:  (files: readonly File[]) => Promise<SdImportOutcome>;
  readonly clearSdCard:  () => Promise<SdImportOutcome>;
  readonly systemFiles:  SystemFilesApi;
  readonly setPaused:    (paused: boolean) => void;
  readonly setFrameSkip: (frames: number) => void;
  readonly setHostCores: (cores: number) => void;
  readonly respondText:  (text: string, accepted: boolean) => void;
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

  const [tools, setTools] = createSignal<HTMLElement>();

  return (
    <div class="voland-shell">
      <header class="voland-header">
        <div class="voland-brand">
          <span class="voland-title">Voland</span>
          <span class="voland-subtitle">Switch emulator in your browser</span>
        </div>
        <dl class="voland-facts">
          <div><dt>CPU</dt><dd>{props.cpuBackend}</dd></div>
          <div><dt>GPU</dt><dd>{props.adapterLabel}</dd></div>
          <div><dt>RAM</dt><dd>{props.guestRamMiB} MiB</dd></div>
          <div><dt>Status</dt><dd class="voland-status">{status()}</dd></div>
        </dl>
      </header>

      <section class="voland-main">
        <div class="voland-hero">
          <div class="voland-screen" data-voland-screen data-testid="screen" />
          <LoadPanel loadGame={props.loadGame} addToSdCard={props.addToSdCard} clearSdCard={props.clearSdCard}
                     systemFiles={props.systemFiles} setPaused={props.setPaused} gpuAdapter={props.adapterLabel}
                     toolsMount={tools} />
          <TextInputDialog respond={props.respondText} />
        </div>

        <aside class="voland-side">
          <section class="voland-panel">
            <h4>Settings</h4>
            <FrameSkipSetting setFrameSkip={props.setFrameSkip} />
            <ThreadsSetting setHostCores={props.setHostCores} />
          </section>
          <div class="voland-tools" ref={setTools} />
          <section class="voland-panel">
            <SavesPanel />
          </section>
          <section class="voland-panel">
            <ControlsLegend />
          </section>
          <details class="voland-panel voland-log">
            <summary>Event log</summary>
            <div class="voland-log-scroll">
              <For each={logs()}>
                {(entry) => (
                  <p class={`log-line log-${entry.level}`}>
                    {`[${entry.level.toUpperCase().padEnd(5, " ")}] ${entry.message}`}
                  </p>
                )}
              </For>
            </div>
          </details>
        </aside>
      </section>
    </div>
  );
}

export default App;
