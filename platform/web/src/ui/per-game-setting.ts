/**
 * The shared behaviour of a per-game setting (src/game-settings.ts): its
 * value follows the running game, a change is remembered for that game
 * (or as the default with none running), and it is applied on every load.
 */
import { type Accessor, createSignal, onCleanup, onMount } from "solid-js";
import {
  type CurrentGame, type GameSettingKey, currentGame, hasOwnSetting, rememberSetting, settingFor, subscribeCurrentGame,
} from "../game-settings";

export interface PerGameSetting {
  readonly value: Accessor<number>;
  readonly game: Accessor<CurrentGame | null>;
  /** Whether the running game has its own value. */
  readonly own: Accessor<boolean>;
  readonly choose: (value: number) => void;
  /** Drops the running game's own value (back to the default). */
  readonly useDefault: () => void;
}

/** `apply` sends a value to the emulator; `valid` filters stored ones. */
export function usePerGameSetting(key: GameSettingKey, fallback: number, valid: (value: number) => boolean,
                                  apply: (value: number) => void): PerGameSetting {
  const effective = (titleId: string | null): number => {
    const value = settingFor(titleId, key, fallback);
    return valid(value) ? value : fallback;
  };
  const [game, setGame] = createSignal<CurrentGame | null>(currentGame());
  const [value, setValue] = createSignal(effective(currentGame()?.titleId ?? null));
  const [own, setOwn] = createSignal(false);
  const sync = (next: CurrentGame | null): void => {
    setGame(next);
    const v = effective(next?.titleId ?? null);
    setValue(v);
    setOwn(next ? hasOwnSetting(next.titleId, key) : false);
    apply(v);
  };
  onMount(() => {
    sync(currentGame());
    onCleanup(subscribeCurrentGame(sync));
  });
  return {
    value, game, own,
    choose: (v: number): void => {
      const titleId = game()?.titleId ?? null;
      rememberSetting(titleId, key, v);
      setValue(v);
      setOwn(titleId !== null);
      apply(v);
    },
    useDefault: (): void => {
      const g = game();
      if (!g) return;
      rememberSetting(g.titleId, key, undefined);
      sync(g);
    },
  };
}
