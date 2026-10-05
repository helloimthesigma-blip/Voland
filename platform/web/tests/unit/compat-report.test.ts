/**
 * The compatibility report text (src/compat-report.ts).
 */
import assert from "node:assert/strict";
import { test } from "node:test";

import { formatReport, problemArea } from "../../src/compat-report.ts";

const CONTEXT = {
  gameName: "silksong", runState: "crashed", runDetail: "stopped at pc=0x10", fps: 0, gpuAdapter: "apple",
  userAgent: "test", settings: "pacing=0", now: new Date(0),
};

test("problems are grouped by area with their counts", () => {
  const text = formatReport({
    titleId: "010013C00E930000", backend: "jit", slices: 100, virtualSeconds: 10, wallSeconds: 20,
    problems: "3\t[WARN ] [ipc] fsp-srv: unimplemented command 61\n1\t[ERROR] [hle] svcBreak reason=0x7\n2\t[WARN ] [ipc] IFileSystem:OpenFile failed: 0x202\n",
    render: { draws: 5 },
  }, CONTEXT);
  assert.match(text, /Game: silksong \(010013C00E930000\)/);
  assert.match(text, /0\.50x/);
  assert.match(text, /Crashes and aborts:\n {8}1x {2}\[hle\] svcBreak reason=0x7/);
  assert.match(text, /Missing services \/ commands:\n {8}3x {2}\[ipc\] fsp-srv: unimplemented command 61/);
  assert.ok(text.indexOf("Crashes") < text.indexOf("Missing services"));
  assert.match(text, /Renderer: draws=5/);
});

test("areas", () => {
  assert.equal(problemArea("[sm] GetServiceHandle(\"nfp:user\"): no HLE service registered"), "Missing services / commands");
  assert.equal(problemArea("[gpu] texture 12 unsupported format"), "Graphics");
  assert.equal(problemArea("something else"), "Other");
});

test("no core: still a report", () => {
  assert.match(formatReport(null, CONTEXT), /^Voland compatibility report/);
});
