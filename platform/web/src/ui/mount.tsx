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
  readonly clearSdCard:  () => Promise<SdImportOutcome>;
  readonly setPaused:    (paused: boolean) => void;
  readonly setFrameSkip: (frames: number) => void;
  readonly setHostCores: (cores: number) => void;
  readonly respondText:  (text: string, accepted: boolean) => void;
}

/** The display canvas (transferred to the GPU worker as an
 * OffscreenCanvas) stays in <body>; this pins its on-screen box to the
 * shell's 16:9 screen element so the guest's frames appear in the UI. */
function attachCanvasToScreen(): void {
  const canvas = document.getElementById("game");
  const screen = document.querySelector<HTMLElement>("[data-voland-screen]");
  if (!canvas || !screen) return;
  const scroller = screen.closest<HTMLElement>(".voland-hero");
  const place = (): void => {
    const box = screen.getBoundingClientRect();
    canvas.style.position = "fixed";
    canvas.style.left = `${box.left}px`;
    canvas.style.top = `${box.top}px`;
    canvas.style.width = `${box.width}px`;
    canvas.style.height = `${box.height}px`;
    canvas.style.zIndex = "5";
    canvas.style.borderRadius = "10px";
    /* Fixed above the scrolling column: clip it to the column's visible
     * part so it never covers the header when scrolled away. */
    if (scroller) {
      const view = scroller.getBoundingClientRect();
      const top = Math.max(0, view.top - box.top), bottom = Math.max(0, box.bottom - view.bottom);
      canvas.style.clipPath = `inset(${top}px 0 ${bottom}px 0 round 10px)`;
      canvas.style.visibility = top >= box.height || bottom >= box.height ? "hidden" : "visible";
    }
  };
  new ResizeObserver(place).observe(screen);
  window.addEventListener("resize", place);
  window.addEventListener("scroll", place, true);
  /* The canvas is not inside the column: hand wheel scrolling over it on. */
  canvas.addEventListener("wheel", (event: WheelEvent) => {
    if (!scroller || document.fullscreenElement) return;
    scroller.scrollBy({ top: event.deltaY, left: 0 });
  }, { passive: true });
  place();
}

export function mountShell(options: MountOptions): void {
  const root = document.getElementById("app-root");
  if (!root) throw new Error("mountShell: #app-root missing from index.html");

  render(() => <App adapterLabel={options.adapterLabel} cpuBackend={options.cpuBackend} guestRamMiB={options.guestRamMiB} loadGame={options.loadGame} addToSdCard={options.addToSdCard} clearSdCard={options.clearSdCard} setPaused={options.setPaused} setFrameSkip={options.setFrameSkip} setHostCores={options.setHostCores} respondText={options.respondText} />, root);

  attachCanvasToScreen();

  const boot = document.getElementById("boot");
  if (boot) {
    boot.classList.add("fade-out");
    window.setTimeout(() => boot.remove(), 260);
  }
}
