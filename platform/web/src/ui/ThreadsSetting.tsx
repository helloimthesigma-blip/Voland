/**
 * platform/web/src/ui/ThreadsSetting.tsx
 *
 * Guest CPU threads: Automatic runs the game's threads on several host
 * cores (docs/PARALLEL.md); Serial runs them all on one, which has less
 * overhead - faster for games whose threads mostly wait on each other
 * (Super Smash Bros. Ultimate fights: about 13 fps serial, 11 on three
 * cores). It applies at once, mid-game. ?cores=N in the URL wins over it.
 * Remembered per browser (a convenience; storage may be unavailable).
 */
import { For, createSignal, onMount } from "solid-js";

interface ThreadsSettingProps {
  /** -1: the default (several cores); 0: serial. */
  readonly setHostCores: (cores: number) => void;
}

const STORAGE_KEY = "voland.hostCores";
const CHOICES: readonly { readonly cores: number; readonly label: string }[] = [
  { cores: -1, label: "Automatic - several cores" },
  { cores: 0, label: "Serial - one core" },
];

function readStored(): number {
  try {
    const value = Number(localStorage.getItem(STORAGE_KEY) ?? "-1");
    return CHOICES.some((c) => c.cores === value) ? value : -1;
  } catch {
    return -1;
  }
}

function urlChooses(): boolean {
  return new URLSearchParams(location.search).get("cores") !== null;
}

function ThreadsSetting(props: ThreadsSettingProps) {
  const [cores, setCores] = createSignal(-1);

  onMount(() => {
    const stored = readStored();
    setCores(stored);
    if (!urlChooses() && stored !== -1) props.setHostCores(stored);
  });

  const choose = (value: number): void => {
    setCores(value);
    props.setHostCores(value);
    try {
      localStorage.setItem(STORAGE_KEY, String(value));
    } catch {
      /* not remembered - still applied */
    }
  };

  return (
    <label class="voland-setting" data-testid="host-cores">
      <span>Guest CPU threads</span>
      <select value={String(cores())} onChange={(e) => choose(Number(e.currentTarget.value))}>
        <For each={CHOICES}>{(c) => <option value={String(c.cores)}>{c.label}</option>}</For>
      </select>
      <small>Serial is faster in some games (Smash Ultimate fights).</small>
    </label>
  );
}

export default ThreadsSetting;
