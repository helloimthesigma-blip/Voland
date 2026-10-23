/**
 * §19 mod repositories (workers/mod-repos.ts): manifest URLs, validation
 * (malformed entries dropped, build ids required for patches and cheats),
 * version order and build-id gating.
 */
import assert from "node:assert/strict";
import { test } from "node:test";

import { blockedReason, compareVersions, manifestUrl, parseManifest } from "../../workers/mod-repos.ts";

const SHA = "a".repeat(64);

test("manifest URL: https only, repository.json appended to a root", () => {
  assert.equal(manifestUrl("https://example.org/mods"), "https://example.org/mods/repository.json");
  assert.equal(manifestUrl("https://example.org/r/index.json"), "https://example.org/r/index.json");
  assert.equal(manifestUrl("http://example.org/mods"), null);
  assert.equal(manifestUrl("http://localhost:8000/"), "http://localhost:8000/repository.json");
  assert.equal(manifestUrl("not a url"), null);
});

test("entries are validated; bad ones are counted, not kept", () => {
  const base = { titleId: "0100000000010000", modId: "m", name: "Mod", version: "1.0", sha256: SHA,
                 artifactUrl: "files/mod.zip", sizeBytes: 10, targetGameVersions: ["1.0.0"] };
  const m = parseManifest("https://example.org/repository.json", {
    schemaVersion: 2, name: "Example",
    entries: [
      { ...base, type: "romfs" },
      { ...base, type: "exefs-patch" },                    /* no buildId */
      { ...base, type: "cheats", buildId: "DEADBEEF01020304" },
      { ...base, type: "javascript" },                     /* no host-code types */
      { ...base, type: "romfs", sha256: "short" },
      { ...base, type: "romfs", artifactUrl: "http://plain.example/mod.zip" },
    ],
  });
  assert.equal(m.name, "Example");
  assert.equal(m.entries.length, 2);
  assert.equal(m.rejected, 4);
  assert.equal(m.entries[0]?.artifactUrl, "https://example.org/files/mod.zip");
  assert.equal(m.entries[1]?.buildId, "deadbeef01020304");
  assert.throws(() => parseManifest("https://x/repository.json", { schemaVersion: 9, entries: [] }), /schemaVersion/);
});

test("versions compare numerically", () => {
  assert.equal(compareVersions("1.10.0", "1.9.2"), 1);
  assert.equal(compareVersions("v2.0", "2.0.0"), 0);
  assert.equal(compareVersions("1.0", "1.0.1"), -1);
});

test("patches need the game's build id to match", () => {
  const entry = { titleId: "0100000000010000", modId: "m", name: "M", version: "1", type: "exefs-patch",
                  targetGameVersions: [], buildId: "deadbeef01020304", artifactUrl: "https://x/m.zip", sha256: SHA, sizeBytes: 1 };
  assert.match(blockedReason(entry, null) ?? "", /load the game/);
  assert.equal(blockedReason(entry, "deadbeef0102030405060708" + "0".repeat(40)), null);
  assert.match(blockedReason(entry, "0".repeat(64)) ?? "", /different game version/);
  assert.equal(blockedReason({ ...entry, type: "romfs", buildId: null }, null), null);
});
