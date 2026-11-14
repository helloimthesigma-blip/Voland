/**
 * Why the emulator's memory could not be allocated, in words for this
 * device. Voland needs one fixed ~5.25 GiB 64-bit (memory64) shared
 * WebAssembly memory (DESIGN.md §4). Every iOS browser - Chrome
 * included - runs on Safari's engine, which cannot provide it; on Android
 * it depends on the phone. Pure: takes what the page reads from navigator.
 */

/* iPadOS reports a Mac user agent; a Mac has no touch points. */
const IPAD_TOUCH_POINTS = 1;

export function isAppleMobile(userAgent: string, maxTouchPoints: number): boolean {
  if (/iPhone|iPad|iPod/.test(userAgent)) return true;
  return /Macintosh/.test(userAgent) && maxTouchPoints > IPAD_TOUCH_POINTS;
}

export function memoryFailureMessage(userAgent: string, maxTouchPoints: number): string {
  if (isAppleMobile(userAgent, maxTouchPoints)) {
    return "Voland cannot run on an iPhone or iPad yet: every iOS browser, Chrome included, uses Safari's engine, " +
      "which cannot give a page the 64-bit memory (~5.25 GB) the emulator needs. Use Chrome or Edge on a computer, " +
      "or Chrome on an Android phone.";
  }
  if (/Android/.test(userAgent)) {
    return "Could not allocate emulator memory (~5.25 GB). This phone or its browser would not give a page that " +
      "much; Voland needs a 64-bit Android phone with a recent Chrome, and more free memory helps.";
  }
  return "Could not allocate emulator memory (~5.25GiB). Your browser or device limits shared memory; use a native build.";
}
