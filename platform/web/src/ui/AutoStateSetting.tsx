/**
 * Automatic save states (src/autostate.ts): how often the running game is
 * saved by itself, for "Continue" in the library and crash recovery.
 * Remembered per browser.
 */
import { For, createSignal } from "solid-js";
import { AUTO_STATE_CHOICES, autoStateMinutes, setAutoStateMinutes } from "../autostate";

function AutoStateSetting() {
  const [minutes, setMinutes] = createSignal(autoStateMinutes());
  const choose = (value: number): void => {
    setMinutes(value);
    setAutoStateMinutes(value);
  };
  return (
    <label class="voland-setting" data-testid="auto-state">
      <span>Resume point</span>
      <select value={String(minutes())} onChange={(e) => choose(Number(e.currentTarget.value))}>
        <For each={AUTO_STATE_CHOICES}>
          {(m) => <option value={String(m)}>{m === 0 ? "Off" : `Every ${m} minutes`}</option>}
        </For>
      </select>
      <small>Saves the whole game by itself (and when you leave the tab), so you can Continue later or go back after a crash. The game pauses for a moment each time.</small>
    </label>
  );
}

export default AutoStateSetting;
