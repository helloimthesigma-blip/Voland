/**
 * platform/web/src/ui/FrameSkipSetting.tsx
 *
 * Frame skip: the reference renderer draws on the CPU (§13), so drawing
 * fewer frames speeds a game up - logic, audio and input still run at full
 * rate; the picture is choppier, and a one-time render landing in a
 * skipped frame may be missing until the game draws it again. The choice
 * is remembered per browser (a convenience; storage may be unavailable).
 */
import { For, createSignal, onMount } from "solid-js";

interface FrameSkipSettingProps {
  readonly setFrameSkip: (frames: number) => void;
}

const STORAGE_KEY = "voland.frameSkip";
const CHOICES: readonly { readonly frames: number; readonly label: string }[] = [
  { frames: 0, label: "Off - every frame" },
  { frames: 1, label: "1 - draw every 2nd frame" },
  { frames: 2, label: "2 - draw every 3rd frame" },
  { frames: 3, label: "3 - draw every 4th frame" },
];

function readStored(): number {
  try {
    const value = Number(localStorage.getItem(STORAGE_KEY) ?? "0");
    return CHOICES.some((c) => c.frames === value) ? value : 0;
  } catch {
    return 0;
  }
}

function FrameSkipSetting(props: FrameSkipSettingProps) {
  const [frames, setFrames] = createSignal(0);

  onMount(() => {
    const stored = readStored();
    setFrames(stored);
    props.setFrameSkip(stored);
  });

  const choose = (value: number): void => {
    setFrames(value);
    props.setFrameSkip(value);
    try {
      localStorage.setItem(STORAGE_KEY, String(value));
    } catch {
      /* not remembered - still applied */
    }
  };

  return (
    <label class="voland-setting" data-testid="frame-skip">
      <span>Frame skip</span>
      <select value={String(frames())} onChange={(e) => choose(Number(e.currentTarget.value))}>
        <For each={CHOICES}>{(c) => <option value={String(c.frames)}>{c.label}</option>}</For>
      </select>
      <small>Faster on slow scenes; the picture gets choppier.</small>
    </label>
  );
}

export default FrameSkipSetting;
