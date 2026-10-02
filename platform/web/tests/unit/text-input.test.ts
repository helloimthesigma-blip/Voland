/**
 * Software-keyboard requests (workers/text-input.ts): the core's U+001F-
 * joined fields parse into a TextInputRequest.
 */
import assert from "node:assert/strict";
import { test } from "node:test";

import { parseTextInputRequest } from "../../workers/text-input.ts";

test("fields parse, numbers and the password flag included", () => {
  const r = parseTextInputRequest(["Name", "Sub", "Type a name", "Link", "8", "1", "1"].join("\u001f"));
  assert.deepEqual(r, { header: "Name", sub: "Sub", guide: "Type a name", initial: "Link", maxLength: 8, minLength: 1, password: true });
});

test("a truncated request is rejected", () => {
  assert.equal(parseTextInputRequest("Name\u001fSub"), null);
});
