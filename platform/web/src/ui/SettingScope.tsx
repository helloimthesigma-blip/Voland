/**
 * Under a per-game setting: whom the value is for - "For Celeste only"
 * (with a way back to the default) while a game with its own value runs,
 * else "All games" or "Changes now apply to <game> only".
 */
import { Show } from "solid-js";
import type { PerGameSetting } from "./per-game-setting";

function SettingScope(props: { readonly setting: PerGameSetting }) {
  return (
    <small class="voland-setting-scope" data-testid="setting-scope">
      <Show when={props.setting.game()} fallback={<>For all games.</>}>
        {(game) => (
          <Show when={props.setting.own()} fallback={<>All games' default; a change here is kept for {game().name} only.</>}>
            For {game().name} only.{" "}
            <button type="button" class="voland-link-button" onClick={() => props.setting.useDefault()}>Use the default</button>
          </Show>
        )}
      </Show>
    </small>
  );
}

export default SettingScope;
