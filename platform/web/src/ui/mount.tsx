/**
 * Dynamically imported from main.ts once both workers report "ready".
 * Splitting the Solid runtime into its own chunk keeps the boot path - the
 * part that runs inside a black screen - small and fast.
 */
import { render } from "solid-js/web";
import type { GameLoadOutcome, SdImportOutcome } from "@bindings/load";
import App from "./App";
import "./shell.css";

export interface MountOptions {
  readonly adapterLabel: string;
  readonly cpuBackend:   string;
  readonly guestRamMiB:  number;
  readonly loadGame:     (file: File) => Promise<GameLoadOutcome>;
  readonly addToSdCard:  (files: readonly File[]) => Promise<SdImportOutcome>;
}

/** The display canvas (transferred to the GPU worker as an
 * OffscreenCanvas) stays in <body>; this pins its on-screen box to the
 * shell's 16:9 screen element so the guest's frames appear in the UI. */
function attachCanvasToScreen(): void {
  const canvas = document.getElementById("game");
  const screen = document.querySelector<HTMLElement>("[data-voland-screen]");
  if (!canvas || !screen) return;
  const place = (): void => {
    const box = screen.getBoundingClientRect();
    canvas.style.position = "fixed";
    canvas.style.left = `${box.left}px`;
    canvas.style.top = `${box.top}px`;
    canvas.style.width = `${box.width}px`;
    canvas.style.height = `${box.height}px`;
    canvas.style.zIndex = "5";
    canvas.style.borderRadius = "10px";
  };
  new ResizeObserver(place).observe(screen);
  window.addEventListener("resize", place);
  window.addEventListener("scroll", place, true);
  place();
}

export function mountShell(options: MountOptions): void {
  const root = document.getElementById("app-root");
  if (!root) throw new Error("mountShell: #app-root missing from index.html");

  render(() => <App adapterLabel={options.adapterLabel} cpuBackend={options.cpuBackend} guestRamMiB={options.guestRamMiB} loadGame={options.loadGame} addToSdCard={options.addToSdCard} />, root);

  attachCanvasToScreen();

  const boot = document.getElementById("boot");
  if (boot) {
    boot.classList.add("fade-out");
    window.setTimeout(() => boot.remove(), 260);
  }
}
