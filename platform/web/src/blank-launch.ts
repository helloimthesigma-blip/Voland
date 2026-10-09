/**
 * "Open in about:blank": Voland in a frame inside a blank tab of its own
 * (launcher-style, the address bar shows about:blank).
 *
 * Works because the blank tab is made by this page: an about:blank
 * document takes its creator's origin and policies (COOP/COEP), so the
 * tab is cross-origin isolated and the same-origin frame in it is too
 * (SharedArrayBuffer, DESIGN.md §16). frame-ancestors 'self' admits it;
 * any other site's frame is still refused. The original tab then leaves
 * for about:blank itself, so two emulators (each a multi-GiB memory) never
 * run at once.
 */

/** True when this page is already the frame of a launcher. */
export function inLauncherFrame(): boolean {
  try {
    return window.self !== window.top;
  } catch {
    return true; /* a cross-origin parent would throw - not possible here, but framed either way */
  }
}

/** Opens the blank tab with Voland framed full-window. False: the browser
 * blocked the new tab (pop-up blocking). */
export function openInBlankTab(): boolean {
  const tab = window.open("about:blank", "_blank");
  if (!tab) return false;
  const doc = tab.document;
  doc.title = "Voland";
  doc.body.style.margin = "0";
  doc.body.style.background = "#000";
  const frame = doc.createElement("iframe");
  frame.src = window.location.href;
  frame.allow = "cross-origin-isolated; fullscreen; gamepad; autoplay; clipboard-read; clipboard-write";
  frame.setAttribute("allowfullscreen", "");
  frame.style.cssText = "position:fixed;inset:0;width:100%;height:100%;border:0;display:block";
  doc.body.appendChild(frame);
  window.location.replace("about:blank");
  return true;
}
