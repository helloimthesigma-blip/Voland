/**
 * "Your files": the Voland folder (src/voland-folder.ts) and a checklist of
 * what Voland has found and what is still missing - games (with their
 * update and DLC), the system files from firmware/, and mods/. Each scan
 * puts the folder's games in the library, adds firmware/'s system files
 * if any are missing, and installs mods/ (again when they change; removed
 * when they leave the folder). A kept folder is scanned again when the
 * page opens if the browser still allows it, else after one click.
 */
import { For, Show, createSignal, onCleanup, onMount } from "solid-js";
import { SYSTEM_FILES, systemFileName } from "@bindings/load";
import type { SystemFilesApi } from "@bindings/load";
import { archiveFiles, installMod, listMods, recordModSource, removeMod, safeModName } from "@workers/mod-store";
import { type LibraryGame, canKeepFolders, forgetGame, shownNameOf } from "../library";
import { exportFileName, exportSaves, titleNames } from "../saves";
import {
  FOLDER_SOURCE, type FolderScan, MODS_DIR, askFolderPermission, folderPermission, inputFiles, keepFolder, keptFolder, modTitleId,
  SAVES_DIR, applyTidy, canTidy, contentHash, pickVolandFolder, scanFolder, setUpFolder, tidyPlan, walkFolder,
  writeSaveBackup,
} from "../voland-folder";

interface FilesPanelProps {
  readonly systemFiles: SystemFilesApi;
  readonly systemIds: () => readonly string[];
  readonly onSystemFilesChanged: () => Promise<void>;
  readonly games: () => readonly LibraryGame[];
  /** A new scan (or null when the folder is let go of): the library
   * remembers its games. */
  readonly onScan: (scan: FolderScan | null, root: FileSystemDirectoryHandle | null) => Promise<void>;
}

const FIRMWARE_SEEN_KEY = "voland-folder-firmware";
const SAVE_BACKUP_KEY = "voland-folder-save-backup";
/** How often saves are copied to saves/ while backups are on (a copy is
 * written only when a save changed). */
const SAVE_BACKUP_MS = 5 * 60 * 1000;

function backupWanted(): boolean {
  try { return localStorage.getItem(SAVE_BACKUP_KEY) === "1"; } catch { return false; }
}

type FolderState =
  | { readonly kind: "none" }
  | { readonly kind: "needs-click"; readonly handle: FileSystemDirectoryHandle }
  | { readonly kind: "scanning"; readonly name: string }
  | { readonly kind: "ready"; readonly name: string; readonly handle: FileSystemDirectoryHandle | null; readonly scan: FolderScan };

interface ModResult {
  readonly installed: readonly string[];
  readonly removed: readonly string[];
  readonly unmatched: readonly string[];
}

function titleIdHex(programId: bigint): string {
  return programId.toString(16).toUpperCase().padStart(16, "0");
}

function formatSize(bytes: number): string {
  return bytes >= 1024 ** 3 ? `${(bytes / 1024 ** 3).toFixed(1)} GB` : `${Math.max(1, Math.round(bytes / 1024 ** 2))} MB`;
}

function FilesPanel(props: FilesPanelProps) {
  const [state, setState] = createSignal<FolderState>({ kind: "none" });
  const [note, setNote] = createSignal<string | null>(null);
  const [mods, setMods] = createSignal<ModResult | null>(null);
  const [usage, setUsage] = createSignal<string | null>(null);
  let folderInput: HTMLInputElement | undefined;
  const [backup, setBackup] = createSignal(backupWanted());
  const [backupNote, setBackupNote] = createSignal<string | null>(null);
  let lastBackupHash = -1;

  /* Copies every save to saves/ if any changed since the last copy and
   * the folder still allows writing (asked for when backups are turned
   * on or the folder is opened with a click). */
  const backUpSaves = async (): Promise<void> => {
    const s = state();
    if (!backup() || s.kind !== "ready" || !s.handle) return;
    if (await folderPermission(s.handle, "readwrite") !== "granted") {
      setBackupNote("Save backups wait for permission to write: click Look again.");
      return;
    }
    const result = await exportSaves();
    if (result.count === 0) return;
    const hash = contentHash(new Uint8Array(result.tar));
    if (hash === lastBackupHash) return;
    const path = await writeSaveBackup(s.handle, exportFileName(new Date()), result.tar);
    if (path) lastBackupHash = hash;
    setBackupNote(path
      ? `${result.count} save(s) backed up to ${path} at ${new Date().toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" })}.`
      : `Could not write to ${SAVES_DIR}/.`);
  };
  const backupTimer = window.setInterval(() => { void backUpSaves(); }, SAVE_BACKUP_MS);
  const onHide = (): void => { if (document.visibilityState === "hidden") void backUpSaves(); };
  document.addEventListener("visibilitychange", onHide);
  onCleanup(() => {
    window.clearInterval(backupTimer);
    document.removeEventListener("visibilitychange", onHide);
  });

  async function toggleBackup(on: boolean, handle: FileSystemDirectoryHandle): Promise<void> {
    if (on && !await askFolderPermission(handle, "readwrite")) {
      setBackupNote("The browser did not allow writing to the folder, so saves are not backed up there.");
      return;
    }
    setBackup(on);
    try { localStorage.setItem(SAVE_BACKUP_KEY, on ? "1" : "0"); } catch { /* storage blocked */ }
    setBackupNote(on ? null : "Save backups to the folder are off.");
    if (on) await backUpSaves();
  }

  const refreshUsage = async (): Promise<void> => {
    const estimate = await navigator.storage?.estimate?.().catch(() => null);
    if (estimate?.usage !== undefined) setUsage(`${formatSize(estimate.usage)} of browser storage used (saves, mods, system files, caches).`);
  };

  /* The games the folder or the library knows a title id for, for
   * matching mods/<name> folders. */
  const knownTitles = (scan: FolderScan): { titleId: string; names: string[] }[] => {
    const known = scan.games.map((g) => ({ titleId: titleIdHex(g.programId), names: [g.name] }));
    for (const game of props.games()) {
      if (game.titleId) known.push({ titleId: game.titleId, names: [shownNameOf(game), game.name] });
    }
    for (const [titleId, name] of Object.entries(titleNames())) known.push({ titleId, names: [name] });
    return known;
  };

  const syncFirmware = async (scan: FolderScan): Promise<void> => {
    const missing = SYSTEM_FILES.some((f) => !props.systemIds().includes(f.id));
    let seen: string | null = null;
    try { seen = localStorage.getItem(FIRMWARE_SEEN_KEY); } catch { /* storage blocked */ }
    if (scan.firmware.length === 0 || !missing || seen === scan.firmwareFingerprint) return;
    setNote(`Reading firmware/ (${scan.firmware.length} files)…`);
    const outcome = await props.systemFiles.add(scan.firmware.map((f) => f.file), null);
    try { localStorage.setItem(FIRMWARE_SEEN_KEY, scan.firmwareFingerprint); } catch { /* storage blocked */ }
    await props.onSystemFilesChanged();
    setNote(outcome.added.length > 0
      ? `Added from firmware/: ${outcome.added.map(systemFileName).join(", ")}.`
      : `firmware/ has none of the missing system files.${outcome.errors.length > 0 ? ` ${outcome.errors.join("; ")}.` : ""}`);
  };

  const syncMods = async (scan: FolderScan): Promise<ModResult> => {
    const known = knownTitles(scan);
    const installed: string[] = [];
    const unmatched: string[] = [];
    const present = new Map<string, Set<string>>();
    for (const mod of scan.mods) {
      const titleId = modTitleId(mod.key, known);
      if (!titleId) {
        unmatched.push(`${MODS_DIR}/${mod.key}`);
        continue;
      }
      const name = safeModName(mod.name);
      present.set(titleId, (present.get(titleId) ?? new Set()).add(name));
      const before = (await listMods(titleId)).find((m) => m.name === name);
      if (before?.source?.repo === FOLDER_SOURCE && before.source.version === mod.fingerprint) continue;
      setNote(`Installing ${mod.key}/${mod.name}…`);
      const first = mod.files[0];
      const files = mod.archive && first ? archiveFiles(mod.name, first.file) : mod.files.map((f) => ({ path: f.path.join("/"), data: f.file }));
      try {
        await installMod(titleId, name, files);
        await recordModSource(titleId, name, { repo: FOLDER_SOURCE, modId: `${mod.key}/${mod.name}`, version: mod.fingerprint });
        installed.push(`${mod.key}/${mod.name}`);
      } catch (e) {
        unmatched.push(`${MODS_DIR}/${mod.key}/${mod.name} (${e instanceof Error ? e.message : String(e)})`);
      }
    }
    /* Mods the folder installed earlier and no longer has. */
    const removed: string[] = [];
    for (const titleId of new Set(known.map((k) => k.titleId))) {
      for (const mod of await listMods(titleId)) {
        if (mod.source?.repo !== FOLDER_SOURCE || present.get(titleId)?.has(mod.name)) continue;
        await removeMod(titleId, mod.name);
        removed.push(mod.source.modId);
      }
    }
    return { installed, removed, unmatched: [...new Set(unmatched)] };
  };

  const applyScan = async (name: string, handle: FileSystemDirectoryHandle | null, scan: FolderScan): Promise<void> => {
    await props.onScan(scan, handle);
    setState({ kind: "ready", name, handle, scan });
    setNote(null);
    await syncFirmware(scan);
    setMods(await syncMods(scan));
    if (note()?.startsWith("Installing")) setNote(null);
    await refreshUsage();
    await backUpSaves();
  };

  const scanKept = async (handle: FileSystemDirectoryHandle): Promise<void> => {
    setState({ kind: "scanning", name: handle.name });
    try {
      const { files, dirs } = await walkFolder(handle);
      await applyScan(handle.name, handle, await scanFolder(files, dirs));
    } catch (e) {
      setState({ kind: "needs-click", handle });
      setNote(`Could not read ${handle.name}: ${e instanceof Error ? e.message : String(e)}`);
    }
  };

  onMount(() => {
    void refreshUsage();
    void keptFolder().then(async (handle) => {
      if (!handle) return;
      if (await folderPermission(handle) === "granted") await scanKept(handle);
      else setState({ kind: "needs-click", handle });
    });
  });

  async function choose(): Promise<void> {
    if (!canKeepFolders()) {
      folderInput?.click();
      return;
    }
    const handle = await pickVolandFolder();
    if (!handle) return;
    await keepFolder(handle);
    await scanKept(handle);
  }

  async function reconnect(handle: FileSystemDirectoryHandle): Promise<void> {
    if (await askFolderPermission(handle, backup() ? "readwrite" : "read")) await scanKept(handle);
    else setNote("The browser did not allow reading the folder.");
  }

  /* "Tidy up": move loose games into games/<name>/ and firmware into
   * firmware/, then forget the loose entries (the moved games come back
   * from the new scan as folder games) and look again. */
  async function tidy(handle: FileSystemDirectoryHandle, scan: FolderScan): Promise<void> {
    const plan = tidyPlan(scan, titleNames());
    setNote(`Moving ${plan.length} file(s)…`);
    const result = await applyTidy(handle, plan);
    for (const game of scan.games) if (game.dir === null && !result.failed.includes(game.program.path.join("/"))) await forgetGame(game.id);
    await scanKept(handle);
    setNote(result.failed.length === 0
      ? `Moved ${result.moved} file(s) into games/ and firmware/.`
      : `Moved ${result.moved} file(s); could not move ${result.failed.join(", ")}.`);
  }

  async function onFolderInput(event: Event & { currentTarget: HTMLInputElement }): Promise<void> {
    const picked = inputFiles(Array.from(event.currentTarget.files ?? []));
    event.currentTarget.value = "";
    if (picked.files.length === 0) return;
    setState({ kind: "scanning", name: picked.name });
    await applyScan(picked.name, null, await scanFolder(picked.files, picked.dirs));
  }

  async function setUp(handle: FileSystemDirectoryHandle): Promise<void> {
    if (await setUpFolder(handle)) await scanKept(handle);
    else setNote("The browser did not allow creating the folders; make games/, firmware/ and mods/ yourself.");
  }

  async function forget(): Promise<void> {
    await keepFolder(null);
    await props.onScan(null, null);
    setState({ kind: "none" });
    setMods(null);
    setNote("Voland no longer reads that folder (nothing in it was changed). Games stay in the library.");
  }

  const ready = (): Extract<FolderState, { kind: "ready" }> | null => {
    const s = state();
    return s.kind === "ready" ? s : null;
  };

  return (
    <section class="voland-panel voland-files" data-testid="files-panel">
      <h4>Your files</h4>
      <input
        ref={(el) => { folderInput = el; el.setAttribute("webkitdirectory", ""); }}
        type="file"
        multiple
        class="voland-load-input"
        data-testid="voland-folder-input"
        onChange={(event) => { void onFolderInput(event); }}
      />
      <Show when={state().kind === "none"}>
        <p class="voland-load-note">
          Pick the folder your decrypted games are in, once. Voland reads every <code>.nca</code> in it, in any
          layout, and sorts them out itself: each game with its update and DLC, and the firmware system files. Mods go
          in <code>mods/&lt;game name&gt;/</code>.
        </p>
        <div class="voland-load-actions">
          <button type="button" class="voland-load-button" data-testid="voland-folder-choose" onClick={() => { void choose(); }}>
            Choose your Voland folder…
          </button>
        </div>
      </Show>
      <Show when={(() => { const s = state(); return s.kind === "needs-click" ? s : null; })()}>
        {(s) => (
          <div class="voland-load-actions">
            <button type="button" class="voland-load-button" data-testid="voland-folder-reconnect"
                    onClick={() => { void reconnect(s().handle); }}>
              Open {s().handle.name}
            </button>
            <small>The browser asks once per visit before Voland may read your folder again.</small>
          </div>
        )}
      </Show>
      <Show when={(() => { const s = state(); return s.kind === "scanning" ? s : null; })()}>
        {(s) => <p class="voland-load-note">Looking through {s().name}…</p>}
      </Show>
      <Show when={ready()}>
        {(r) => (
          <div data-testid="voland-folder-ready">
            <p class="voland-files-folder">
              <strong>{r().name}</strong>
              <span class="voland-ok"> connected</span>
            </p>
            <Show when={r().scan.missingDirs.length > 0 && r().scan.games.length === 0 && r().scan.firmware.length === 0}>
              <p class="voland-missing" data-testid="voland-folder-missing-dirs">
                Nothing found yet. Voland can make games/, firmware/ and mods/ folders here to keep things tidy (optional:
                files anywhere in the folder are found too).
              </p>
              <Show when={r().handle}>
                {(handle) => (
                  <button type="button" class="voland-load-button voland-load-secondary" data-testid="voland-folder-setup"
                          onClick={() => { void setUp(handle()); }}>
                    Create the missing folders
                  </button>
                )}
              </Show>
            </Show>
            <Show when={r().handle && canTidy() && tidyPlan(r().scan, titleNames()).length > 0 ? r().handle : null}>
              {(handle) => (
                <div class="voland-setting">
                  <button type="button" class="voland-load-button voland-load-secondary" data-testid="voland-folder-tidy"
                          onClick={() => { void tidy(handle(), r().scan); }}>
                    Tidy up: sort {tidyPlan(r().scan, titleNames()).length} file(s) into folders
                  </button>
                  <small> Each game goes in games/&lt;its name&gt;/ with its update and DLC, firmware in firmware/. Files are
                    only moved, never deleted.</small>
                </div>
              )}
            </Show>
            <ul class="voland-checklist">
              <li data-testid="files-games">
                <Show when={r().scan.games.length > 0} fallback={<span class="voland-missing">No games found yet: put your decrypted game's .nca files anywhere in this folder.</span>}>
                  <span class="voland-ok">{r().scan.games.length} game(s)</span>
                  <ul>
                    <For each={r().scan.games}>
                      {(g) => (
                        <li>
                          {g.name} <code>{titleIdHex(g.programId)}</code>
                          {" - "}{g.hasUpdate ? "update" : "no update"}{g.dlc > 0 ? `, ${g.dlc} DLC` : ""}
                        </li>
                      )}
                    </For>
                  </ul>
                </Show>
              </li>
              <li data-testid="files-system">
                System files:{" "}
                <For each={SYSTEM_FILES}>
                  {(f, i) => (
                    <>
                      {i() > 0 ? ", " : ""}
                      <span class={props.systemIds().includes(f.id) ? "voland-ok" : "voland-missing"}>{f.name}</span>
                    </>
                  )}
                </For>
                <Show when={r().scan.firmware.length === 0 && SYSTEM_FILES.some((f) => !props.systemIds().includes(f.id))}>
                  <small> - add your decrypted firmware NCAs anywhere in the folder (only Mario Kart 8, Smash and Animal Crossing need them).</small>
                </Show>
              </li>
              <li data-testid="files-mods">
                {r().scan.mods.length === 0 ? "No mods in mods/." : `${r().scan.mods.length} mod(s) in mods/`}
                <Show when={mods()}>
                  {(m) => (
                    <>
                      {m().installed.length > 0 ? `; installed ${m().installed.join(", ")}` : ""}
                      {m().removed.length > 0 ? `; removed ${m().removed.join(", ")} (gone from the folder)` : ""}
                      <Show when={m().unmatched.length > 0}>
                        <span class="voland-missing" data-testid="files-mods-unmatched">
                          {" "}- no game found for {m().unmatched.join(", ")}: name the folder after the game (as the library
                          shows it) or its title id.
                        </span>
                      </Show>
                    </>
                  )}
                </Show>
              </li>
              <Show when={r().handle}>
                {(handle) => (
                  <li data-testid="files-saves">
                    <label>
                      <input type="checkbox" checked={backup()} data-testid="files-save-backup"
                             onChange={(e) => { void toggleBackup(e.currentTarget.checked, handle()); }} />
                      {" "}Back up saves to {SAVES_DIR}/ (one file a day, written only when a save changed)
                    </label>
                    <Show when={backupNote()}>{(text) => <small> - {text()}</small>}</Show>
                  </li>
                )}
              </Show>
              <Show when={r().scan.encrypted.length > 0}>
                <li class="voland-missing" data-testid="files-encrypted">
                  Still encrypted (left out; decrypt them with your own tools first): {r().scan.encrypted.join(", ")}
                </li>
              </Show>
              <Show when={r().scan.orphans.length > 0}>
                <li class="voland-missing" data-testid="files-orphans">
                  No game for: {r().scan.orphans.join(", ")} (an update or DLC needs its game somewhere in the folder).
                </li>
              </Show>
            </ul>
            <div class="voland-load-actions">
              <button type="button" class="voland-load-button voland-load-secondary" data-testid="voland-folder-rescan"
                      onClick={() => { const h = r().handle; if (h) void reconnect(h); else folderInput?.click(); }}>
                Look again
              </button>
              <button type="button" class="voland-load-button voland-load-secondary" onClick={() => { void choose(); }}>
                Other folder…
              </button>
              <button type="button" class="voland-load-button voland-load-secondary" data-testid="voland-folder-forget"
                      onClick={() => { void forget(); }}>
                Stop using it
              </button>
            </div>
          </div>
        )}
      </Show>
      <Show when={note()}>{(text) => <p class="voland-load-note" data-testid="files-note">{text()}</p>}</Show>
      <Show when={usage()}>{(text) => <p class="voland-load-note"><small>{text()}</small></p>}</Show>
    </section>
  );
}

export default FilesPanel;
