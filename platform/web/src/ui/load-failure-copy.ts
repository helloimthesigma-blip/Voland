/**
 * User-facing wording for a failed game load. Kept apart from the Solid
 * component so it is unit-testable under plain Node (npm test).
 *
 * The encrypted-input case is §1.6's required error path: Voland never
 * decrypts, so the only useful thing to say is how the user decrypts
 * their own dump with separate tools - docs/DUMP.md.
 */
import type { LoadFailure } from "@bindings/load";

/** docs/DUMP.md as published in the project repository. */
export const DUMPING_GUIDE_URL = "https://github.com/voland-emu/Voland/blob/main/docs/DUMP.md";

export interface LoadFailureCopy {
  readonly heading:     string;
  readonly explanation: string;
  /** Link to the dumping guide, or null when it would not help. */
  readonly guideUrl:    string | null;
}

export function describeLoadFailure(failure: LoadFailure): LoadFailureCopy {
  switch (failure.reason) {
    case "encrypted-input":
      return {
        heading: "This file is encrypted",
        explanation:
          "Voland only reads decrypted NCA files. NSP and XCI files, and NCAs " +
          "copied straight off a console, are encrypted. Decrypt your own dump " +
          "on your PC with a separate tool, then load the decrypted Program NCA.",
        guideUrl: DUMPING_GUIDE_URL,
      };
    case "unsupported-content":
      return {
        heading: "This NCA can't be started",
        explanation:
          "The file is decrypted, but it isn't a bootable Program NCA. Load the " +
          "NCA that holds the game's executable (ExeFS with main.npdm).",
        guideUrl: DUMPING_GUIDE_URL,
      };
    case "read-failed":
      return {
        heading: "The file couldn't be read",
        explanation: "The browser lost access to the file. Pick it again.",
        guideUrl: null,
      };
    case "internal":
      return {
        heading: "The emulator couldn't load the file",
        explanation: "Something went wrong inside Voland. The event log has details.",
        guideUrl: null,
      };
  }
}
