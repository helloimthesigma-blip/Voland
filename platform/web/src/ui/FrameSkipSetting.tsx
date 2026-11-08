/**
 * platform/web/src/ui/FrameSkipSetting.tsx
 *
 * Frame skip: the reference renderer draws on the CPU (§13), so drawing
 * fewer frames speeds a game up - logic, audio and input still run at full
 * rate; the picture is choppier. Renders into textures the last drawn
 * frame did not draw (one-time ones) still run in skipped frames. Changed
 * while a game runs, it is remembered for that game (src/game-settings.ts).
 */
import { For } from "solid-js";
import { usePerGameSetting } from "./per-game-setting";
import SettingScope from "./SettingScope";

interface FrameSkipSettingProps {
  readonly setFrameSkip: (frames: number) => void;
}

const CHOICES: readonly { readonly frames: number; readonly label: string }[] = [
  { frames: 0, label: "Off - every frame" },
  { frames: 1, label: "1 - draw every 2nd frame" },
  { frames: 2, label: "2 - draw every 3rd frame" },
  { frames: 3, label: "3 - draw every 4th frame" },
];

function FrameSkipSetting(props: FrameSkipSettingProps) {
  const setting = usePerGameSetting("frameSkip", 0, (v) => CHOICES.some((c) => c.frames === v), props.setFrameSkip);

  return (
    <label class="voland-setting" data-testid="frame-skip">
      <span>Frame skip</span>
      <select value={String(setting.value())} onChange={(e) => setting.choose(Number(e.currentTarget.value))}>
        <For each={CHOICES}>{(c) => <option value={String(c.frames)}>{c.label}</option>}</For>
      </select>
      <small>Faster on slow scenes; the picture gets choppier.</small>
      <SettingScope setting={setting} />
    </label>
  );
}

export default FrameSkipSetting;
