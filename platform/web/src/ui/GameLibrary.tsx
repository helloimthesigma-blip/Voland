/**
 * The game library: one tile per game the player has loaded (src/library.ts).
 * Click a tile to play; the browser may ask once per session to let Voland
 * read the file again. Drop game files onto the library to add them.
 */
import { For, Show, createSignal } from "solid-js";
import type { LibraryGame } from "../library";

interface GameLibraryProps {
  readonly games: readonly LibraryGame[];
  readonly busy: boolean;
  readonly onLaunch: (game: LibraryGame) => void;
  readonly onForget: (game: LibraryGame) => void;
  readonly onDropFiles: (items: DataTransferItemList) => void;
}

const TILE_HUES = 12;

/** A stable accent per game, so tiles are told apart at a glance. Pure. */
function hueOf(id: string): number {
  let h = 0;
  for (let i = 0; i < id.length; i++) h = (h * 31 + id.charCodeAt(i)) >>> 0;
  return (h % TILE_HUES) * (360 / TILE_HUES);
}

function initialsOf(name: string): string {
  const words = name.split(/[\s_\-.]+/).filter(Boolean);
  const letters = words.length > 1 ? `${words[0]?.[0] ?? ""}${words[1]?.[0] ?? ""}` : name.slice(0, 2);
  return letters.toUpperCase();
}

function formatSize(bytes: number): string {
  return bytes >= 1024 ** 3 ? `${(bytes / 1024 ** 3).toFixed(1)} GB` : `${Math.max(1, Math.round(bytes / 1024 ** 2))} MB`;
}

function playedLabel(game: LibraryGame): string {
  if (game.lastPlayedAt === null) return "not played yet";
  const days = Math.floor((Date.now() - game.lastPlayedAt) / 86_400_000);
  return days <= 0 ? "played today" : days === 1 ? "played yesterday" : `played ${days} days ago`;
}

function GameLibrary(props: GameLibraryProps) {
  const [dragging, setDragging] = createSignal(false);
  return (
    <div
      class="voland-library"
      classList={{ dragging: dragging() }}
      data-testid="library"
      onDragOver={(e) => { e.preventDefault(); setDragging(true); }}
      onDragLeave={(e) => { if (e.currentTarget === e.target) setDragging(false); }}
      onDrop={(e) => {
        e.preventDefault();
        setDragging(false);
        if (e.dataTransfer) props.onDropFiles(e.dataTransfer.items);
      }}
    >
      <Show when={props.games.length > 0} fallback={
        <p class="voland-library-empty" data-testid="library-empty">
          Your games appear here after you load them once, ready to launch with one click. You can also drop game files here.
        </p>
      }>
        <div class="voland-library-grid">
          <For each={props.games}>
            {(game) => (
              <div class="voland-game" data-testid="library-game" data-game={game.id}>
                <button type="button" class="voland-game-play" disabled={props.busy} title={`Play ${game.name}`}
                  onClick={() => props.onLaunch(game)}>
                  <span class="voland-game-art" style={{ "--hue": `${hueOf(game.id)}` }}>{initialsOf(game.name)}</span>
                  <span class="voland-game-name">{game.name}</span>
                  <span class="voland-game-meta">
                    {formatSize(game.size)} · {playedLabel(game)}
                    <Show when={!game.handle}> · asks for the file</Show>
                  </span>
                </button>
                <button type="button" class="voland-game-forget" title="Remove from library (the file is not deleted)"
                  aria-label={`Remove ${game.name} from the library`} data-testid="library-forget"
                  onClick={() => props.onForget(game)}>×</button>
              </div>
            )}
          </For>
        </div>
      </Show>
    </div>
  );
}

export default GameLibrary;
