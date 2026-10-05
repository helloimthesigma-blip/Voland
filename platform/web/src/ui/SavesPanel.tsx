/**
 * Save data (§15): every game's user save directory, as stored in this
 * browser. Browse a save's files, download one, replace or delete it, drop
 * files or whole folders in, and back everything up to a file. Edits apply
 * at once: the save is rewritten (src/saves.ts -> the CPU worker loads it
 * into the core and stores it), so the game sees it the next time it reads.
 */
import { For, Show, createSignal, onMount } from "solid-js";
import type { StoredSave } from "@bindings/protocol";
import {
  type SaveTree, addSaveDirectory, describeSaveName, normalizeSavePath, parseSaveArchive,
  putSaveFile, removeSavePath, sortedSaveEntries, writeSaveArchive,
} from "@bindings/save-archive";
import { writeTar } from "@workers/save-store";
import { deleteSave, exportFileName, exportSaves, importSaves, listSaves, putSave, titleNames } from "../saves";

interface SaveView {
  readonly name: string;
  readonly title: string;
  readonly detail: string;
  readonly tree: SaveTree | null; /* null: damaged */
}

const IN_USE_NOTE =
  "The running game has this save open, so it was not changed. Reload the page, edit the save before starting the game, then load the game.";

function formatBytes(bytes: number): string {
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
}

function treeBytes(tree: SaveTree): number {
  let total = 0;
  for (const data of tree.values()) total += data?.byteLength ?? 0;
  return total;
}

function toView(save: StoredSave, names: Readonly<Record<string, string>>): SaveView {
  const identity = describeSaveName(save.name);
  const program = identity?.programId ?? "?";
  const title = program === "0000000000000000"
    ? "Unassigned save (from before per-game saves; the next game you load takes it over)"
    : names[program] ?? `Title ${program}`;
  const detail = `${identity?.typeLabel ?? "Save"} save · ${program}`;
  return { name: save.name, title, detail, tree: parseSaveArchive(new Uint8Array(save.archive)) };
}

function download(bytes: Uint8Array, fileName: string, type = "application/octet-stream"): void {
  const url = URL.createObjectURL(new Blob([new Uint8Array(bytes)], { type }));
  const link = document.createElement("a");
  link.href = url;
  link.download = fileName;
  link.click();
  window.setTimeout(() => URL.revokeObjectURL(url), 10_000);
}

function baseName(path: string): string {
  return path.slice(path.lastIndexOf("/") + 1);
}

function parentOf(path: string): string {
  const at = path.lastIndexOf("/");
  return at <= 0 ? "/" : path.slice(0, at);
}

function joinPath(directory: string, relative: string): string | null {
  return normalizeSavePath(`${directory}/${relative}`);
}

/* Dropped folders: walk FileSystemEntry trees into (relative path, file). */
async function entryFiles(entry: FileSystemEntry, prefix: string): Promise<(readonly [string, File])[]> {
  if (entry.isFile) {
    const file = await new Promise<File>((resolve, reject) => (entry as FileSystemFileEntry).file(resolve, reject));
    return [[`${prefix}${entry.name}`, file]];
  }
  if (!entry.isDirectory) return [];
  const reader = (entry as FileSystemDirectoryEntry).createReader();
  const children: FileSystemEntry[] = [];
  for (;;) {
    const batch = await new Promise<FileSystemEntry[]>((resolve, reject) => reader.readEntries(resolve, reject));
    if (batch.length === 0) break;
    children.push(...batch);
  }
  const nested = await Promise.all(children.map((child) => entryFiles(child, `${prefix}${entry.name}/`)));
  return nested.flat();
}

async function droppedFiles(transfer: DataTransfer): Promise<(readonly [string, File])[]> {
  const entries = Array.from(transfer.items)
    .map((item) => (item.kind === "file" ? item.webkitGetAsEntry?.() ?? null : null))
    .filter((entry): entry is FileSystemEntry => entry !== null);
  if (entries.length === 0) return Array.from(transfer.files).map((file) => [file.name, file] as const);
  const all = await Promise.all(entries.map((entry) => entryFiles(entry, "")));
  return all.flat();
}

function SaveBrowser(props: { readonly save: SaveView; readonly onChanged: () => void; readonly note: (text: string) => void }) {
  const [tree, setTree] = createSignal<SaveTree>(props.save.tree ?? new Map());
  const [target, setTarget] = createSignal("/");
  const [dragging, setDragging] = createSignal(false);
  const [busy, setBusy] = createSignal(false);
  /* What the last edit did, shown right above the files, and which rows it touched. */
  const [status, setStatus] = createSignal<{ readonly ok: boolean; readonly text: string } | null>(null);
  const [changed, setChanged] = createSignal<ReadonlySet<string>>(new Set());
  let addInput: HTMLInputElement | undefined;
  let replaceInput: HTMLInputElement | undefined;
  let replacing = "";

  async function commit(next: SaveTree, done: string, touched: readonly string[] = []): Promise<void> {
    setBusy(true);
    setStatus({ ok: true, text: "Saving…" });
    const ok = await putSave(props.save.name, writeSaveArchive(next));
    setBusy(false);
    if (ok) {
      setTree(next);
      setChanged(new Set(touched));
      const text = `✓ ${done} Saved - the game reads it the next time it loads this save.`;
      setStatus({ ok: true, text });
      props.note(done);
    } else {
      setChanged(new Set<string>());
      setStatus({ ok: false, text: `✗ Not changed. ${IN_USE_NOTE}` });
      props.note(IN_USE_NOTE);
    }
  }

  async function addFiles(files: readonly (readonly [string, File])[], directory: string): Promise<void> {
    let next = tree();
    const added: string[] = [];
    for (const [relative, file] of files) {
      const path = joinPath(directory, relative);
      if (!path) continue;
      next = putSaveFile(next, path, new Uint8Array(await file.arrayBuffer()));
      added.push(path);
    }
    if (added.length === 0) return;
    await commit(next, `Added ${added.length} file${added.length === 1 ? "" : "s"} to ${directory}.`, added);
  }

  async function onDrop(event: DragEvent, directory: string): Promise<void> {
    event.preventDefault();
    event.stopPropagation();
    setDragging(false);
    if (!event.dataTransfer) return;
    await addFiles(await droppedFiles(event.dataTransfer), directory);
  }

  function onNewFolder(): void {
    const name = window.prompt(`New folder in ${target()}:`);
    if (!name) return;
    const path = joinPath(target(), name);
    if (!path) return;
    void commit(addSaveDirectory(tree(), path), `Created ${path}.`, [path]);
  }

  function downloadAll(): void {
    const files = new Map<string, Uint8Array>();
    const folder = props.save.title.replace(/[^A-Za-z0-9._-]+/g, "-").slice(0, 40) || "save";
    for (const [path, data] of tree()) if (data) files.set(`${folder}${path}`, data);
    download(writeTar(files), `${folder}-save.tar`, "application/x-tar");
  }

  const entries = () => sortedSaveEntries(tree());
  const depth = (path: string): number => path.split("/").length - 2;

  return (
    <div
      class="voland-save-browser"
      classList={{ dragging: dragging() }}
      data-testid="save-browser"
      onDragOver={(e) => { e.preventDefault(); setDragging(true); }}
      onDragLeave={(e) => { if (e.currentTarget === e.target) setDragging(false); }}
      onDrop={(e) => void onDrop(e, target())}
    >
      <Show when={props.save.tree === null}>
        <p class="voland-load-note">This stored save is damaged and cannot be shown. Deleting it lets the game make a new one.</p>
      </Show>
      <Show when={status()}>
        {(s) => <p class="voland-save-status" classList={{ failed: !s().ok }} data-testid="save-status" role="status">{s().text}</p>}
      </Show>
      <div class="voland-save-files" role="tree">
        <div
          class="voland-save-row voland-save-dir"
          classList={{ target: target() === "/" }}
          role="treeitem"
          onClick={() => setTarget("/")}
          onDrop={(e) => void onDrop(e, "/")}
        >
          <span class="voland-save-name">/ (save root)</span>
          <span class="voland-save-size">{formatBytes(treeBytes(tree()))}</span>
          <span />
        </div>
        <For each={entries()}>
          {([path, data]) => (
            <div
              class="voland-save-row"
              classList={{ "voland-save-dir": data === null, target: target() === path, changed: changed().has(path) }}
              role="treeitem"
              data-path={path}
              style={{ "padding-left": `${12 + 16 * (depth(path) + 1)}px` }}
              onClick={() => setTarget(data === null ? path : parentOf(path))}
              onDrop={(e) => void onDrop(e, data === null ? path : parentOf(path))}
            >
              <span class="voland-save-name">
                {data === null ? `${baseName(path)}/` : baseName(path)}
                <Show when={changed().has(path)}><span class="voland-save-badge">updated</span></Show>
              </span>
              <span class="voland-save-size">{data ? formatBytes(data.byteLength) : ""}</span>
              <span class="voland-save-actions">
                <Show when={data}>
                  {(bytes) => (
                    <>
                      <button type="button" title="Download this file" onClick={(e) => { e.stopPropagation(); download(bytes(), baseName(path)); }}>Download</button>
                      <button type="button" title="Replace with a file from your computer" disabled={busy()}
                        onClick={(e) => { e.stopPropagation(); replacing = path; replaceInput?.click(); }}>Replace…</button>
                    </>
                  )}
                </Show>
                <button type="button" disabled={busy()} data-testid="save-delete-path"
                  onClick={(e) => {
                    e.stopPropagation();
                    if (data === null && !window.confirm(`Delete ${path} and everything in it?`)) return;
                    void commit(removeSavePath(tree(), path), `Deleted ${path}.`);
                  }}>Delete</button>
              </span>
            </div>
          )}
        </For>
      </div>
      <p class="voland-save-drop">
        Drop files or folders here to add them to <code>{target()}</code> (a file with the same name is replaced).
        Click a folder to drop into it.
      </p>
      <div class="voland-player-bar">
        <button type="button" disabled={busy()} data-testid="save-add-files" onClick={() => addInput?.click()}>Add files…</button>
        <button type="button" disabled={busy()} onClick={onNewFolder}>New folder</button>
        <button type="button" data-testid="save-download" onClick={downloadAll}>Download this save</button>
        <button type="button" disabled={busy()} data-testid="save-delete"
          onClick={() => {
            if (!window.confirm(`Delete this whole save (${props.save.title})? This cannot be undone - back it up first.`)) return;
            void deleteSave(props.save.name).then((ok) => {
              props.note(ok ? "Save deleted." : IN_USE_NOTE);
              if (ok) props.onChanged();
            });
          }}>Delete save</button>
      </div>
      <input ref={addInput} type="file" multiple hidden data-testid="save-add-input"
        onChange={(e) => {
          const files = Array.from(e.currentTarget.files ?? []).map((file) => [file.name, file] as const);
          e.currentTarget.value = "";
          void addFiles(files, target());
        }} />
      <input ref={replaceInput} type="file" hidden
        onChange={(e) => {
          const file = e.currentTarget.files?.[0];
          e.currentTarget.value = "";
          if (!file || !replacing) return;
          const path = replacing;
          void file.arrayBuffer().then((buffer) =>
            commit(putSaveFile(tree(), path, new Uint8Array(buffer)), `Replaced ${path} with ${file.name} (${formatBytes(buffer.byteLength)}).`, [path]));
        }} />
    </div>
  );
}

function SavesPanel() {
  const [note, setNote] = createSignal<string | null>(null);
  const [saves, setSaves] = createSignal<readonly SaveView[]>([]);
  const [open, setOpen] = createSignal<string | null>(null);
  let input: HTMLInputElement | undefined;

  async function refresh(): Promise<void> {
    const names = titleNames();
    setSaves((await listSaves()).map((save) => toView(save, names)));
  }
  onMount(() => void refresh());

  async function onExport(): Promise<void> {
    const result = await exportSaves();
    if (result.count === 0) {
      setNote("No game saves stored yet.");
      return;
    }
    download(new Uint8Array(result.tar), exportFileName(new Date()), "application/x-tar");
    setNote(`Downloaded ${result.count} save${result.count === 1 ? "" : "s"}.`);
  }

  async function onImport(event: Event & { currentTarget: HTMLInputElement }): Promise<void> {
    const file = event.currentTarget.files?.[0];
    event.currentTarget.value = "";
    if (!file) return;
    const result = await importSaves(file);
    setNote(result.rejected
      ? `Imported ${result.imported}; ${result.rejected} could not be used (damaged, or the running game has them open - load the game again).`
      : `Imported ${result.imported} save${result.imported === 1 ? "" : "s"}.`);
    await refresh();
  }

  return (
    <details class="voland-saves" data-testid="saves" onToggle={(e) => { if (e.currentTarget.open) void refresh(); }}>
      <summary>Save data (each game's user save directory)</summary>
      <p class="voland-load-note">
        Saves are kept in this browser automatically each time a game saves. Open one to see its files, download or
        replace them, or drag files and folders in. Edit saves before starting the game (a running game may have them
        open). Back everything up to a file now and then: clearing site data deletes them.
      </p>
      <div class="voland-player-bar">
        <button type="button" data-testid="saves-refresh" onClick={() => void refresh()}>Refresh</button>
        <button type="button" data-testid="saves-export" onClick={() => void onExport()}>Back up all saves</button>
        <button type="button" data-testid="saves-import" onClick={() => input?.click()}>Restore a backup…</button>
        <input ref={input} type="file" accept=".tar" hidden data-testid="saves-import-input" onChange={(e) => void onImport(e)} />
      </div>
      <Show when={note()}>{(text) => <p class="voland-load-note" data-testid="saves-note">{text()}</p>}</Show>
      <Show when={saves().length === 0}>
        <p class="voland-load-note" data-testid="saves-empty">No saves yet. Start a game and save once; its save directory appears here.</p>
      </Show>
      <For each={saves()}>
        {(save) => (
          <div class="voland-save" data-testid="save-entry" data-save={save.name}>
            <button type="button" class="voland-save-head" onClick={() => setOpen(open() === save.name ? null : save.name)}>
              <span class="voland-save-title">{save.title}</span>
              <span class="voland-save-meta">
                {save.detail} · {save.tree ? `${[...save.tree.values()].filter((d) => d !== null).length} files, ${formatBytes(treeBytes(save.tree))}` : "damaged"}
              </span>
            </button>
            <Show when={open() === save.name}>
              <SaveBrowser save={save} note={setNote} onChanged={() => { setOpen(null); void refresh(); }} />
            </Show>
          </div>
        )}
      </For>
    </details>
  );
}

export default SavesPanel;
