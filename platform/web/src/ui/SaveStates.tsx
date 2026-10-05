/**
 * Save states for the running game: freeze the whole machine into a state
 * and jump back to it later (src/savestates.ts -> workers/savestate.ts).
 * Unlike the game's own saves this works anywhere - mid-fight, in a menu,
 * right after a crash - and does not depend on how the game saves.
 */
import { For, Show, createSignal, onMount } from "solid-js";
import type { SavestateInfo } from "@bindings/protocol";
import { deleteState, listStates, loadState, saveState } from "../savestates";

interface SaveStatesProps {
  readonly titleId: string;
}

function formatGameTime(seconds: number): string {
  const s = Math.floor(seconds);
  const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), r = s % 60;
  return h > 0 ? `${h}:${String(m).padStart(2, "0")}:${String(r).padStart(2, "0")}` : `${m}:${String(r).padStart(2, "0")}`;
}

function formatWhen(ms: number): string {
  const date = new Date(ms);
  const sameDay = new Date().toDateString() === date.toDateString();
  return sameDay ? date.toLocaleTimeString([], { hour: "numeric", minute: "2-digit" })
    : date.toLocaleString([], { month: "short", day: "numeric", hour: "numeric", minute: "2-digit" });
}

function SaveStates(props: SaveStatesProps) {
  const [states, setStates] = createSignal<readonly SavestateInfo[]>([]);
  const [busy, setBusy] = createSignal<"saving" | "loading" | null>(null);
  const [status, setStatus] = createSignal<{ readonly ok: boolean; readonly text: string } | null>(null);

  const refresh = async (): Promise<void> => {
    setStates((await listStates()).filter((s) => s.titleId === props.titleId));
  };
  onMount(() => void refresh());

  async function onSave(): Promise<void> {
    setBusy("saving");
    setStatus({ ok: true, text: "Saving the whole machine… the game pauses for a few seconds." });
    const result = await saveState();
    setBusy(null);
    setStatus(result.ok && result.info
      ? { ok: true, text: `✓ State saved (${(result.info.bytes / 2 ** 20).toFixed(0)} MB).` }
      : { ok: false, text: `✗ Could not save a state: ${result.error}` });
    await refresh();
  }

  async function onLoad(state: SavestateInfo): Promise<void> {
    setBusy("loading");
    setStatus({ ok: true, text: "Loading the state…" });
    const result = await loadState(state.id);
    setBusy(null);
    setStatus(result.ok ? { ok: true, text: `✓ Back at ${formatWhen(state.createdAt)}.` } : { ok: false, text: `✗ ${result.error}` });
  }

  return (
    <div class="voland-states" data-testid="save-states">
      <div class="voland-player-bar">
        <button type="button" data-testid="state-save" disabled={busy() !== null} onClick={() => void onSave()}>
          {busy() === "saving" ? "Saving state…" : "Save state"}
        </button>
      </div>
      <Show when={status()}>
        {(s) => <p class="voland-save-status" classList={{ failed: !s().ok }} data-testid="state-status" role="status">{s().text}</p>}
      </Show>
      <Show when={states().length > 0}>
        <ul class="voland-state-list">
          <For each={states()}>
            {(state) => (
              <li class="voland-state" data-testid="state-entry">
                <span class="voland-state-when">{formatWhen(state.createdAt)}</span>
                <span class="voland-state-meta">game time {formatGameTime(state.virtualSeconds)} · {(state.bytes / 2 ** 20).toFixed(0)} MB</span>
                <span class="voland-save-actions">
                  <button type="button" data-testid="state-load" disabled={busy() !== null} onClick={() => void onLoad(state)}>Load</button>
                  <button type="button" data-testid="state-delete" disabled={busy() !== null}
                    onClick={() => void deleteState(state.id).then(refresh)}>Delete</button>
                </span>
              </li>
            )}
          </For>
        </ul>
      </Show>
    </div>
  );
}

export default SaveStates;
