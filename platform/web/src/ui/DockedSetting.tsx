/**
 * platform/web/src/ui/DockedSetting.tsx
 *
 * Docked or handheld: the operation mode games see (emulator_set_docked).
 * Docked reports a 1920x1080 display and Boost performance, and games
 * render larger for it (Tears of the Kingdom 1600x900 instead of
 * 1280x720): sharper, more work for the GPU. A game reads it as it
 * starts, so a change made while it runs applies from its next start
 * (emulator_set_docked says why); it is remembered for that game
 * (src/game-settings.ts).
 */
import { For } from "solid-js";
import { usePerGameSetting } from "./per-game-setting";
import SettingScope from "./SettingScope";

interface DockedSettingProps {
  readonly setDocked: (docked: boolean) => void;
}

const HANDHELD = 0;
const DOCKED = 1;

const CHOICES: readonly { readonly value: number; readonly label: string }[] = [
  { value: HANDHELD, label: "Handheld - 1280x720" },
  { value: DOCKED, label: "Docked - 1920x1080" },
];

function DockedSetting(props: DockedSettingProps) {
  const setting = usePerGameSetting("docked", HANDHELD, (v) => CHOICES.some((c) => c.value === v),
                                    (v) => props.setDocked(v === DOCKED));

  return (
    <label class="voland-setting" data-testid="docked">
      <span>Console mode</span>
      <select value={String(setting.value())} onChange={(e) => setting.choose(Number(e.currentTarget.value))}>
        <For each={CHOICES}>{(c) => <option value={String(c.value)}>{c.label}</option>}</For>
      </select>
      <small>Docked games draw at a higher resolution: sharper, but more work for the GPU. Applies when the game starts.</small>
      <SettingScope setting={setting} />
    </label>
  );
}

export default DockedSetting;
