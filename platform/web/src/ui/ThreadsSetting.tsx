/**
 * platform/web/src/ui/ThreadsSetting.tsx
 *
 * Guest CPU threads: Automatic runs the game's threads on several host
 * cores (docs/PARALLEL.md), except for games known to run faster serially
 * (cpu.worker.ts SERIAL_TITLES: Super Smash Bros. Ultimate); Serial runs
 * them all on one, which has less overhead - faster for games whose
 * threads mostly wait on each other. It applies at once, mid-game.
 * ?cores=N in the URL wins over it. Changed while a game runs, it is
 * remembered for that game (src/game-settings.ts).
 */
import { For } from "solid-js";
import { usePerGameSetting } from "./per-game-setting";
import SettingScope from "./SettingScope";

interface ThreadsSettingProps {
  /** -1: the default (several cores); 0: serial. */
  readonly setHostCores: (cores: number) => void;
}

const CHOICES: readonly { readonly cores: number; readonly label: string }[] = [
  { cores: -1, label: "Automatic" },
  { cores: 0, label: "Serial - one core" },
];

function urlChooses(): boolean {
  return new URLSearchParams(location.search).get("cores") !== null;
}

function ThreadsSetting(props: ThreadsSettingProps) {
  /* ?cores=N in the URL wins: nothing is sent while it is there. */
  const setting = usePerGameSetting("hostCores", -1, (v) => CHOICES.some((c) => c.cores === v),
    (v) => { if (!urlChooses()) props.setHostCores(v); });

  return (
    <label class="voland-setting" data-testid="host-cores">
      <span>Guest CPU threads</span>
      <select value={String(setting.value())} onChange={(e) => setting.choose(Number(e.currentTarget.value))}>
        <For each={CHOICES}>{(c) => <option value={String(c.cores)}>{c.label}</option>}</For>
      </select>
      <small>Automatic picks serial for games that run faster that way (Smash Ultimate).</small>
      <SettingScope setting={setting} />
    </label>
  );
}

export default ThreadsSetting;
