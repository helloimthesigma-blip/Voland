/**
 * The memory-failure message (src/device-support.ts): iOS browsers,
 * Chrome on iOS included, are told why; Android and computers get their own.
 */
import assert from "node:assert/strict";
import { test } from "node:test";

import { isAppleMobile, memoryFailureMessage } from "../../src/device-support.ts";

const IPHONE_CHROME = "Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) CriOS/131.0.6778.73 Mobile/15E148 Safari/604.1";
const IPAD_SAFARI = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 Safari/605.1.15";
const ANDROID_CHROME = "Mozilla/5.0 (Linux; Android 14; Pixel 7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Mobile Safari/537.36";
const MAC_CHROME = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36";

test("Chrome on an iPhone and Safari on an iPad are Apple mobile; a Mac is not", () => {
  assert.equal(isAppleMobile(IPHONE_CHROME, 5), true);
  assert.equal(isAppleMobile(IPAD_SAFARI, 5), true);
  assert.equal(isAppleMobile(MAC_CHROME, 0), false);
  assert.equal(isAppleMobile(ANDROID_CHROME, 5), false);
});

test("each device gets its own explanation", () => {
  assert.match(memoryFailureMessage(IPHONE_CHROME, 5), /Chrome included/);
  assert.match(memoryFailureMessage(ANDROID_CHROME, 5), /Android phone/);
  assert.match(memoryFailureMessage(MAC_CHROME, 0), /native build/);
});
