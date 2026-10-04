/**
 * Game saves (§15): they are kept in this browser automatically whenever a
 * game saves; this offers a backup file and a way to bring one back.
 */
import { Show, createSignal } from "solid-js";
import { exportFileName, exportSaves, importSaves } from "../saves";

function SavesPanel() {
  const [note, setNote] = createSignal<string | null>(null);
  let input: HTMLInputElement | undefined;

  async function onExport(): Promise<void> {
    const result = await exportSaves();
    if (result.count === 0) {
      setNote("No game saves stored yet.");
      return;
    }
    const url = URL.createObjectURL(new Blob([result.tar], { type: "application/x-tar" }));
    const link = document.createElement("a");
    link.href = url;
    link.download = exportFileName(new Date());
    link.click();
    window.setTimeout(() => URL.revokeObjectURL(url), 10_000);
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
  }

  return (
    <div class="voland-saves" data-testid="saves">
      <p class="voland-load-note">
        Game saves are kept in this browser automatically each time a game saves. Back them up to a file,
        or bring a backup back:
      </p>
      <div class="voland-player-bar">
        <button type="button" data-testid="saves-export" onClick={() => void onExport()}>Back up saves</button>
        <button type="button" data-testid="saves-import" onClick={() => input?.click()}>Restore saves…</button>
        <input ref={input} type="file" accept=".tar" hidden data-testid="saves-import-input" onChange={(e) => void onImport(e)} />
      </div>
      <Show when={note()}>{(text) => <p class="voland-load-note" data-testid="saves-note">{text()}</p>}</Show>
    </div>
  );
}

export default SavesPanel;
