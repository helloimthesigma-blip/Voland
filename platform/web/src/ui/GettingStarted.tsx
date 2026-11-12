/**
 * "How to use Voland": the steps from an empty page to playing, for
 * someone who just opened the site. Open until the player closes it once
 * (remembered per browser); the steps name the buttons and panels they
 * point at, so keep them in step with LoadPanel, FilesPanel and the side
 * panels' headings.
 */
import { DUMPING_GUIDE_URL } from "./load-failure-copy";

const CLOSED_KEY = "voland-help-closed";

function closedBefore(): boolean {
  try { return localStorage.getItem(CLOSED_KEY) === "1"; } catch { return false; }
}

function remember(open: boolean): void {
  try {
    if (open) localStorage.removeItem(CLOSED_KEY);
    else localStorage.setItem(CLOSED_KEY, "1");
  } catch { /* storage blocked: it opens again next visit */ }
}

function GettingStarted() {
  return (
    <details class="voland-panel voland-help" open={!closedBefore()} data-testid="getting-started"
             onToggle={(e) => remember(e.currentTarget.open)}>
      <summary>How to use Voland</summary>
      <ol>
        <li>
          <strong>Bring your own game.</strong> Voland comes with no games. Dump a game you own from your Switch and
          decrypt it on your computer with your own tools; Voland reads decrypted NCA files and never decrypts
          anything itself.{" "}
          <a href={DUMPING_GUIDE_URL} target="_blank" rel="noopener noreferrer">How to dump and decrypt</a>
        </li>
        <li>
          <strong>Load it.</strong> Press <em>Load game folder…</em> and pick the folder with your game's decrypted
          NCAs: Voland finds the right one and brings the name, icon, update and DLC. <em>Load NCA or homebrew NRO…</em>{" "}
          takes a single Program NCA. After that the game waits in the library with one-click launch.
        </li>
        <li>
          <strong>Give it a moment.</strong> The first start of a big game can take a few minutes before the title
          screen; the frame counter shows it is working.
        </li>
        <li>
          <strong>Play.</strong> Use the keyboard or a gamepad; <em>Controls</em> lists every button and lets you
          change them. <em>Fullscreen</em> sits under the screen once a game runs.
        </li>
        <li>
          <strong>Your progress.</strong> The game's own saves are kept in this browser automatically (back them
          up from <em>Save data</em>). <em>Save states</em> freeze the game anywhere, and <em>Continue</em> on a
          game's tile takes you back to where you were.
        </li>
      </ol>
      <p class="voland-load-note">
        Optional: choose a <em>Voland folder</em> under <em>Your files</em> to keep games, system files and mods in
        one place; some games need <em>System files</em> from your own console; <em>Mods</em> adds mods you have.
        Just curious? <em>Run the demo</em> starts a tiny built-in program. Chrome or Edge on a computer works best.
      </p>
    </details>
  );
}

export default GettingStarted;
