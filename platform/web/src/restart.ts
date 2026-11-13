/**
 * Restarting the running game from elsewhere on the page (the Mods tab
 * after a mod is added: mods apply at load). LoadPanel registers how to
 * start the game it last launched from the library; anything else asks
 * through restartGame(). Nothing crosses to the workers here - it is the
 * same launch the library tile does.
 */
import { createSignal } from "solid-js";

type Restarter = { readonly titleId: string; readonly restart: () => Promise<boolean> };

const [restarter, setRestarter] = createSignal<Restarter | null>(null);

/** LoadPanel: the game now running and how to start it again (null when
 * there is none, or it was not launched from the library). */
export function registerRestart(next: Restarter | null): void {
  setRestarter(next);
}

/** The title id that restartGame() would start again, or null (reactive). */
export function restartableTitle(): string | null {
  return restarter()?.titleId ?? null;
}

/** Starts the running game again; true once it is loaded. */
export function restartGame(): Promise<boolean> {
  return restarter()?.restart() ?? Promise.resolve(false);
}
