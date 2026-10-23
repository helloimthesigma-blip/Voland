/**
 * The readable event log (workers/log-feed.ts): raw core feed lines become
 * plain-language entries, families fold into one entry plus counts, and
 * the status line reads speed and fps from two samples.
 */
import assert from "node:assert/strict";
import { test } from "node:test";

import { FeedFolder, explainLine, statusLine } from "../../workers/log-feed.ts";

test("notes pass through; known problems are explained", () => {
  assert.deepEqual(explainLine("NOTE first picture from the game (1280x720)")?.entry,
                   { level: "info", message: "first picture from the game (1280x720)" });
  const shader = explainLine("WARN [gpu] pixel program 636ab30 not translated: global memory (LDG)");
  assert.equal(shader?.family, "untranslated:global memory (LDG)");
  assert.match(shader?.entry.message ?? "", /graphics: a pixel shader uses global memory \(LDG\)/);
  assert.equal(explainLine("WARN [ipc] IFileSystem:OpenFile failed: 0x202")?.entry.level, "info");
  assert.equal(explainLine("ERROR something odd")?.entry.level, "error");
  assert.equal(explainLine("garbage"), null);
});

test("a family is told once, then counted", () => {
  const folder = new FeedFolder();
  const shown = folder.take([
    "WARN [gpu] pixel program 1 not translated: global memory (LDG)",
    "WARN [gpu] pixel program 2 not translated: global memory (LDG)",
    "WARN [gpu] pixel program 3 not translated: global memory (LDG)",
    "NOTE the game saved its progress (1 save so far)",
  ]);
  assert.equal(shown.length, 2);
  assert.deepEqual(folder.summary(), [{ level: "info", message: "2 more shaders could not be translated" }]);
  assert.deepEqual(folder.summary(), []);
});

test("status line: fps and speed over the interval", () => {
  const line = statusLine({ wallSeconds: 30, virtualSeconds: 30, frames: 900 }, { wallSeconds: 60, virtualSeconds: 54, frames: 1620 });
  assert.equal(line, "playing for 1:00: 24 fps, 80% speed");
  assert.match(statusLine({ wallSeconds: 0, virtualSeconds: 0, frames: 0 }, { wallSeconds: 30, virtualSeconds: 30, frames: 1800 }), /60 fps, full speed/);
  assert.match(statusLine({ wallSeconds: 0, virtualSeconds: 0, frames: 0 }, { wallSeconds: 30, virtualSeconds: 15, frames: 300 }), /slow motion/);
});
