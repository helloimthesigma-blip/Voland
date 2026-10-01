/**
 * Unit tests for bindings/load.ts and the load-failure wording - the
 * Result-code mapping behind §1.6's encrypted-input error path.
 */
import assert from "node:assert/strict";
import { test } from "node:test";

import { CoreResult, formatTitleId, loadFailureFromResult, readCString } from "../../bindings/load.ts";
import { DUMPING_GUIDE_URL, describeLoadFailure } from "../../src/ui/load-failure-copy.ts";

test("CoreResult mirrors core/common/result.h", () => {
  assert.equal(CoreResult.Ok, 0);
  assert.equal(CoreResult.IoError, 4);
  assert.equal(CoreResult.EncryptedInput, 8);
});

test("RESULT_ENCRYPTED_INPUT maps to encrypted-input and keeps the core message", () => {
  const failure = loadFailureFromResult(CoreResult.EncryptedInput, "NCA header magic missing");
  assert.deepEqual(failure, { reason: "encrypted-input", message: "NCA header magic missing" });
});

test("structural rejections map to unsupported-content", () => {
  for (const code of [CoreResult.InvalidArgument, CoreResult.NotFound, CoreResult.NotImplemented]) {
    assert.equal(loadFailureFromResult(code, "").reason, "unsupported-content");
  }
});

test("I/O failure maps to read-failed; anything else is internal", () => {
  assert.equal(loadFailureFromResult(CoreResult.IoError, "").reason, "read-failed");
  assert.equal(loadFailureFromResult(CoreResult.OutOfMemory, "").reason, "internal");
  assert.equal(loadFailureFromResult(999, "").reason, "internal");
});

test("encrypted input links the dumping guide", () => {
  const copy = describeLoadFailure({ reason: "encrypted-input", message: "x" });
  assert.equal(copy.guideUrl, DUMPING_GUIDE_URL);
  assert.match(copy.heading, /encrypted/i);
  assert.match(DUMPING_GUIDE_URL, /docs\/DUMP\.md$/);
});

test("read and internal failures do not point at the guide", () => {
  assert.equal(describeLoadFailure({ reason: "read-failed", message: "" }).guideUrl, null);
  assert.equal(describeLoadFailure({ reason: "internal", message: "" }).guideUrl, null);
});

test("formatTitleId pads to 16 upper-case hex digits", () => {
  assert.equal(formatTitleId(0x0100000000010000n), "0100000000010000");
  assert.equal(formatTitleId(0xabcn), "0000000000000ABC");
  assert.equal(formatTitleId(-1n), "FFFFFFFFFFFFFFFF");
});

test("readCString stops at NUL, honours the bound, and treats 0 as empty", () => {
  const memory = new WebAssembly.Memory({ initial: 1, maximum: 1, shared: true });
  const bytes = new TextEncoder().encode("encrypted\0tail");
  new Uint8Array(memory.buffer).set(bytes, 64);

  assert.equal(readCString(memory.buffer, 64, 1024), "encrypted");
  assert.equal(readCString(memory.buffer, 64, 4), "encr");
  assert.equal(readCString(memory.buffer, 0, 1024), "");
});
