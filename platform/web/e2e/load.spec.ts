import { existsSync, readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";

import { expect, test } from "@playwright/test";

/**
 * Game-load e2e: the §1.6 encrypted-input error path and the decrypted
 * happy path, end to end through the real core (Main -> CPU worker ->
 * wasm64 core -> FileReaderSync reads -> back). Needs two build outputs:
 *   - the staged core:  cmake --build --preset web
 *     (copies switch_core.{js,wasm} to platform/web/public/core)
 *   - the fixture NCA:  ctest --preset native-noop
 *     (writes build/native-noop/fixtures/synthetic_program.nca; every
 *     byte synthetic - no Nintendo data, tests/program_nca_fixture.c)
 * Without them these tests skip with the reason; boot.spec.ts still runs.
 */

const CORE_JS = fileURLToPath(new URL("../public/core/switch_core.js", import.meta.url));
const FIXTURE_NCA = process.env["VOLAND_PROGRAM_NCA_FIXTURE"] ??
  fileURLToPath(new URL("../../../build/native-noop/fixtures/synthetic_program.nca", import.meta.url));

/* Must match tests/program_nca_fixture.c FIXTURE_PROGRAM_ID. */
const EXPECTED_TITLE_ID = "0100000000042000";

/* Big enough for nca_open to read the whole 0xC00 header. */
const FAKE_ENCRYPTED_BYTES = 0x4000;

test.skip(!existsSync(CORE_JS), "core not staged: run `cmake --build --preset web` first");

test.beforeEach(async ({ page }) => {
  await page.goto("/");
  await expect(page.getByTestId("load-panel")).toBeVisible({ timeout: 20_000 });
});

test("an encrypted file shows the dumping-guide error", async ({ page }) => {
  // Deterministic noise: what an encrypted NCA header looks like to a
  // parser that never decrypts (no NCA3 magic at 0x200).
  const noise = Buffer.alloc(FAKE_ENCRYPTED_BYTES);
  for (let i = 0; i < noise.length; i++) noise[i] = (i * 151 + 7) & 0xff;

  await page.getByTestId("load-input").setInputFiles({
    name: "game.nsp",
    mimeType: "application/octet-stream",
    buffer: noise,
  });

  const error = page.getByTestId("load-error");
  await expect(error).toBeVisible();
  await expect(error).toHaveAttribute("data-reason", "encrypted-input");
  await expect(error).toContainText("docs/DUMP.md");
  await expect(page.getByTestId("dumping-guide-link")).toHaveAttribute("href", /docs\/DUMP\.md$/);
});

test("a decrypted program NCA loads and reports its title id", async ({ page }) => {
  test.skip(!existsSync(FIXTURE_NCA), "fixture missing: run `ctest --preset native-noop` first");

  await page.getByTestId("load-input").setInputFiles({
    name: "synthetic_program.nca",
    mimeType: "application/octet-stream",
    buffer: readFileSync(FIXTURE_NCA),
  });

  const success = page.getByTestId("load-success");
  await expect(success).toBeVisible();
  await expect(success).toContainText(EXPECTED_TITLE_ID);
});
