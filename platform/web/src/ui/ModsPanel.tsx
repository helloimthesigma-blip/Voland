/**
 * Mods (§19): install mod folders or .zip / .7z archives for a game (found
 * from a title id in the mod's paths when it has one, else the game picked
 * here), restart the running game to apply them, turn them on and off, set which wins when two change the same file, and toggle the
 * running game's cheats. Voland ships no mods and links to no mod sites;
 * it reads the usual layouts (romfs/, exefs/, cheats/, *.ips, *.pchtxt,
 * atmosphere/contents/<title id>/...) and game-update / DLC NCAs. Mod changes apply at the next load;
 * cheats switch at once.
 */
import { For, Show, createSignal, onCleanup, onMount } from "solid-js";
import {
  ARCHIVE_PATTERN, type ModFileSource, type ModInfo, archiveFiles, installMod, listMods, moveMod, peekTitleId, removeMod,
  safeModName, safeParts, setModEnabled,
} from "@workers/mod-store";
import { restartGame, restartableTitle } from "../restart";
import { titleNames } from "../saves";
import { type RunningMods, runningMods, setCheatEnabled, subscribeRunningMods } from "../mods";
import ModRepos from "./ModRepos";
import { FOLDER_SOURCE } from "../voland-folder";

function formatBytes(bytes: number): string {
  if (bytes < 1024 * 1024) return `${Math.max(1, Math.round(bytes / 1024))} KB`;
  if (bytes < 1024 ** 3) return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
  return `${(bytes / 1024 ** 3).toFixed(2)} GB`;
}

/* Dropped folders: FileSystemEntry trees into (relative path, file). */
async function entryFiles(entry: FileSystemEntry, prefix: string): Promise<ModFileSource[]> {
  if (entry.isFile) {
    const file = await new Promise<File>((resolve, reject) => (entry as FileSystemFileEntry).file(resolve, reject));
    return [{ path: `${prefix}${entry.name}`, data: file }];
  }
  if (!entry.isDirectory) return [];
  const reader = (entry as FileSystemDirectoryEntry).createReader();
  const children: FileSystemEntry[] = [];
  for (;;) {
    const batch = await new Promise<FileSystemEntry[]>((resolve, reject) => reader.readEntries(resolve, reject));
    if (batch.length === 0) break;
    children.push(...batch);
  }
  const out: ModFileSource[] = [];
  for (const child of children) out.push(...await entryFiles(child, `${prefix}${entry.name}/`));
  return out;
}

/** Splits files picked with a folder input ("Mod/romfs/a.bin") into the
 * mod name and its files relative to the mod. Pure. */
function folderPick(files: readonly File[]): { readonly name: string; readonly files: readonly ModFileSource[] } | null {
  const first = files[0];
  if (!first) return null;
  const rel = (f: File): string => (f as File & { readonly webkitRelativePath?: string }).webkitRelativePath || f.name;
  const name = safeParts(rel(first))[0] ?? "mod";
  return { name, files: files.map((f) => ({ path: safeParts(rel(f)).slice(1).join("/") || f.name, data: f })) };
}

function ModsPanel() {
  /* Stored names, read again when a game loads (localStorage is not
   * reactive: a first load left the panel saying "load a game once"). */
  const [names, setNames] = createSignal<Readonly<Record<string, string>>>(titleNames());
  const [running, setRunning] = createSignal<RunningMods | null>(runningMods());
  const titles = (): readonly string[] => {
    const ids = new Set(Object.keys(names()));
    const now = running()?.titleId;
    if (now) ids.add(now); /* the running game, even before its name is stored */
    return [...ids].sort((a, b) => (names()[a] ?? a).localeCompare(names()[b] ?? b));
  };
  const [title, setTitle] = createSignal<string>(running()?.titleId ?? titles()[0] ?? "");
  const [mods, setMods] = createSignal<readonly ModInfo[]>([]);
  const [busy, setBusy] = createSignal<string | null>(null);
  const [note, setNote] = createSignal<string | null>(null);

  const refresh = async (): Promise<void> => {
    const id = title();
    setMods(id ? await listMods(id) : []);
  };

  onMount(() => {
    void refresh();
    const off = subscribeRunningMods((state) => {
      const loaded = state !== null && state.titleId !== running()?.titleId;
      if (loaded) setNames(titleNames());
      setRunning(state);
      /* A game just loaded: show its mods and cheats. */
      if (state && (loaded || !title())) {
        setTitle(state.titleId);
        void refresh();
      }
    });
    onCleanup(off);
  });

  /* The game a mod was last installed for, while it waits for a restart. */
  const [installedFor, setInstalledFor] = createSignal<string | null>(null);

  const install = async (name: string, source: AsyncIterable<ModFileSource> | Iterable<ModFileSource>): Promise<void> => {
    const { titleId: found, files } = await peekTitleId(source);
    const id = found ?? title();
    if (!id) {
      setNote(`${name}: Voland could not tell which game it is for. Start the game once, then add the mod again.`);
      return;
    }
    if (id !== title()) {
      setTitle(id);
      if (!names()[id]) setNames({ ...names(), [id]: id });
    }
    setBusy(`Installing ${name}...`);
    try {
      const written = await installMod(id, name, files, (n) => setBusy(`Installing ${name}: ${n} file(s)...`));
      const game = names()[id] ?? id;
      setNote(`Installed ${safeModName(name)} for ${game} (${written} file(s)).` +
        (restartableTitle() === id ? "" : " It applies the next time you start the game."));
      setInstalledFor(id);
    } catch (e) {
      setNote(`Could not install ${name}: ${e instanceof Error ? e.message : String(e)}`);
    } finally {
      setBusy(null);
      await refresh();
    }
  };

  /* "Add mod…": archives, and update / DLC NCAs (each a mod of its own). */
  const installPicked = async (files: readonly File[]): Promise<void> => {
    for (const file of files) {
      if (ARCHIVE_PATTERN.test(file.name)) await install(file.name, archiveFiles(file.name, file));
      else if (/\.nca$/i.test(file.name)) await install(file.name, [{ path: file.name, data: file }]);
      else setNote(`${file.name}: a mod is a .zip / .7z, a folder, or an update / DLC .nca.`);
    }
  };

  const onDrop = async (items: DataTransferItemList): Promise<void> => {
    const entries = Array.from(items).map((item) => item.webkitGetAsEntry()).filter((e): e is FileSystemEntry => e !== null);
    for (const entry of entries) {
      if (entry.isDirectory) {
        const files = await entryFiles(entry, "");
        await install(entry.name, files.map((f) => ({ path: safeParts(f.path).slice(1).join("/"), data: f.data })));
      } else if (ARCHIVE_PATTERN.test(entry.name)) {
        const [file] = await entryFiles(entry, "");
        if (file) await install(entry.name, archiveFiles(entry.name, file.data));
      } else if (/\.nca$/i.test(entry.name)) {
        const [file] = await entryFiles(entry, "");
        if (file) await install(entry.name, [{ path: entry.name, data: file.data }]);
      } else {
        setNote(`${entry.name}: drop a mod folder, a .zip / .7z archive or an update / DLC .nca.`);
      }
    }
  };

  const act = async (fn: () => Promise<void>): Promise<void> => {
    await fn();
    await refresh();
  };

  return (
    <div class="voland-mods" data-testid="mods-panel"
         onDragOver={(e) => e.preventDefault()}
         onDrop={(e) => { e.preventDefault(); if (e.dataTransfer) void onDrop(e.dataTransfer.items); }}>
      <h4>Mods</h4>
      <ol class="voland-mod-steps">
        <li>Download a mod for your game: a <code>.zip</code>, <code>.7z</code> or folder (an update or DLC is its decrypted <code>.nca</code>).</li>
        <li>Press <em>Add mod…</em> or drop it here. Voland works out which game it is for.</li>
        <li>Restart the game to play with it.</li>
      </ol>
      <div class="voland-setting">
        <label class="voland-load-button">
          Add mod…
          <input type="file" hidden accept=".zip,.7z,.nca" multiple data-testid="mods-add-zip"
                 onChange={(e) => {
                   const files = Array.from(e.currentTarget.files ?? []);
                   e.currentTarget.value = "";
                   void installPicked(files);
                 }} />
        </label>
        <label class="voland-load-button voland-load-secondary">
          Add mod folder…
          <input type="file" hidden data-testid="mods-add-folder" {...{ webkitdirectory: true }}
                 onChange={(e) => {
                   const pick = folderPick(Array.from(e.currentTarget.files ?? []));
                   e.currentTarget.value = "";
                   if (pick) void install(pick.name, pick.files);
                 }} />
        </label>
      </div>
      <Show when={busy()}>{(text) => <p class="voland-load-note">{text()}</p>}</Show>
      <Show when={note()}>{(text) => <p class="voland-load-note" data-testid="mods-note">{text()}</p>}</Show>
      <Show when={installedFor() !== null && installedFor() === restartableTitle()}>
        <div class="voland-setting">
          <button type="button" class="voland-load-button" data-testid="mods-restart"
                  onClick={() => { setInstalledFor(null); void restartGame(); }}>
            Restart {names()[installedFor() ?? ""] ?? "the game"} now
          </button>
        </div>
      </Show>
      <Show when={titles().length > 0}>
        <label class="voland-setting">
          <span>Game</span>
          <select value={title()} data-testid="mods-game"
                  onChange={(e) => { setTitle(e.currentTarget.value); void refresh(); }}>
            <For each={titles()}>{(id) => <option value={id}>{names()[id] ?? id} ({id})</option>}</For>
          </select>
        </label>
        <small>Mods higher in the list win when two change the same file.</small>
        <ul class="voland-mod-list">
          <For each={mods()} fallback={<li class="voland-load-note">No mods installed for this game.</li>}>
            {(mod, index) => (
              <li class="voland-mod" data-testid="mod-row">
                <label>
                  <input type="checkbox" checked={mod.enabled}
                         onChange={(e) => void act(() => setModEnabled(title(), mod.name, e.currentTarget.checked))} />
                  <span class="voland-mod-name">{mod.name}</span>
                </label>
                <span class="voland-mod-meta">{mod.files} file(s) · {formatBytes(mod.bytes)}{mod.source?.repo === FOLDER_SOURCE ? " · from your Voland folder" : mod.source ? ` · ${mod.source.version} from a repository` : ""}</span>
                <span class="voland-mod-actions">
                  <button type="button" title="Higher priority" disabled={index() === 0}
                          onClick={() => void act(() => moveMod(title(), mod.name, -1))}>▲</button>
                  <button type="button" title="Lower priority" disabled={index() === mods().length - 1}
                          onClick={() => void act(() => moveMod(title(), mod.name, 1))}>▼</button>
                  <button type="button" title="Delete this mod from the browser"
                          onClick={() => { if (confirm(`Delete the mod "${mod.name}"?`)) void act(() => removeMod(title(), mod.name)); }}>×</button>
                </span>
              </li>
            )}
          </For>
        </ul>
        <ModRepos titleId={title()} installed={mods()} onInstalled={() => void refresh()}
                  buildId={running()?.titleId === title() ? running()?.buildId ?? null : null} />
        <Show when={running() && running()?.titleId === title() ? running() : null}>
          {(state) => (
            <div class="voland-cheats">
              <h5>Running now</h5>
              <p class="voland-load-note">
                {state().mods.length > 0 ? `Mods applied: ${state().mods.join(", ")}.` : "No mods applied at this load."}
              </p>
              <Show when={state().cheats.length > 0}>
                <ul class="voland-mod-list">
                  <For each={state().cheats}>
                    {(cheat, index) => (
                      <li class="voland-mod">
                        <label>
                          <input type="checkbox" checked={cheat.enabled || cheat.master} disabled={cheat.master}
                                 onChange={(e) => setCheatEnabled(index(), e.currentTarget.checked)} />
                          <span class="voland-mod-name">{cheat.name || "(unnamed cheat)"}{cheat.master ? " (master code)" : ""}</span>
                        </label>
                      </li>
                    )}
                  </For>
                </ul>
              </Show>
            </div>
          )}
        </Show>
      </Show>
    </div>
  );
}

export default ModsPanel;
