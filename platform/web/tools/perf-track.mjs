#!/usr/bin/env node
/**
 * Perf regression tracker: measures one commit with tools/perf.mjs and
 * appends a row to a CSV (docs/perf/silksong.csv in this checkout).
 *
 *   node tools/perf-track.mjs --game FILE [--ref origin/main]
 *        [--worktree DIR] [--csv FILE] [--preset web] [--url-params ...]
 *        [--threshold 0.10]
 *
 * - Checks the commit out (detached) in a dedicated worktree (default
 *   ../../../Voland-perf beside this checkout), builds the shipping web core
 *   (`cmake --preset web --fresh`, so a stale cache never keeps an old
 *   backend), installs the web dependencies if needed, and
 *   runs this checkout's perf.mjs against it (--web-dir) twice:
 *     (a) the title: warm up to slice 860,000, measure 30 s;
 *     (b) the gameplay recipe (the --press list in perf.mjs's header) with load phases
 *         boot/title/menus/newgame to slice 4,800,000, then measure 30 s.
 * - Appends commit, date, load averages, worker seconds to the title and to
 *   gameplay, fps, slices/s, ticks/s, run-time wasm modules and the
 *   CPU-normalised figures: renderer CPU seconds per virtual (guest) second
 *   for the load, the title and gameplay. Wall-time figures swing with the
 *   machine's load; CPU per virtual second much less.
 * - Compares the CPU-normalised figures with the previous row and prints
 *   `REGRESSION <metric> +N%` for any that grew by more than --threshold.
 *   Rows compare only with rows of the same preset and --url-params.
 *   The last line is `TRACKED <sha> <ok|regression>`.
 */
import { execFileSync, spawnSync } from "node:child_process";
import { appendFileSync, existsSync, mkdirSync, readFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const TOOLS = dirname(fileURLToPath(import.meta.url));
const REPO = resolve(TOOLS, "../../..");
const PRESSES = [860000, 940000, 1020000, 3000000, 3100000, 3200000].flatMap((slice) => ["--press", `${slice}:z:3000`]);
const PHASES = "boot:0,title:860000,menus:1100000,newgame:3000000,end:4800000";

/* CSV columns, in order. "*_cpu_per_vs" are the normalised figures. */
const COLUMNS = [
  "commit", "date", "preset", "backend", "host_cores", "load_avg_start", "load_avg_end",
  "worker_s_to_title", "worker_s_to_gameplay", "cpu_s_to_gameplay", "virtual_s_to_gameplay",
  "title_fps", "title_slices_per_s", "title_ticks_per_s",
  "gameplay_fps", "gameplay_slices_per_s", "gameplay_ticks_per_s", "jit_modules_to_gameplay",
  "load_cpu_per_vs", "title_cpu_per_vs", "gameplay_cpu_per_vs",
];
const NORMALISED = ["load_cpu_per_vs", "title_cpu_per_vs", "gameplay_cpu_per_vs"];

function parseArgs(argv) {
  const opts = {
    game: "", ref: "origin/main", worktree: resolve(REPO, "../Voland-perf"),
    csv: join(REPO, "docs/perf/silksong.csv"), preset: "web", urlParams: "", threshold: 0.1,
  };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    const next = () => argv[++i] ?? "";
    if (a === "--game") opts.game = next();
    else if (a === "--ref") opts.ref = next();
    else if (a === "--worktree") opts.worktree = resolve(next());
    else if (a === "--csv") opts.csv = resolve(next());
    else if (a === "--preset") opts.preset = next();
    else if (a === "--url-params") opts.urlParams = next();
    else if (a === "--threshold") opts.threshold = Number(next());
    else throw new Error(`unknown argument ${a}`);
  }
  if (!opts.game || !existsSync(opts.game)) throw new Error("--game FILE is required");
  return opts;
}

function sh(command, cwd) {
  const r = spawnSync("bash", ["-c", command], { cwd, stdio: "inherit" });
  if (r.status !== 0) throw new Error(`failed (${r.status}): ${command}`);
}

function git(args, cwd = REPO) {
  return execFileSync("git", args, { cwd, encoding: "utf8" }).trim();
}

function runPerf(opts, webDir, extra, jsonFile) {
  const args = [join(TOOLS, "perf.mjs"), "--web-dir", webDir, "--game", opts.game, "--json", jsonFile,
                "--out-dir", dirname(jsonFile), ...(opts.urlParams ? ["--url-params", opts.urlParams] : []), ...extra];
  const r = spawnSync("node", args, { cwd: webDir, stdio: "inherit" });
  if (r.status !== 0) throw new Error(`perf.mjs failed (${r.status})`);
  return JSON.parse(readFileSync(jsonFile, "utf8"));
}

/* The last row measured with the same preset and page parameters. */
function previousRow(csv, preset) {
  if (!existsSync(csv)) return null;
  const lines = readFileSync(csv, "utf8").trim().split("\n");
  const header = lines[0].split(",");
  for (let i = lines.length - 1; i >= 1; i--) {
    const row = Object.fromEntries(header.map((h, k) => [h, lines[i].split(",")[k]]));
    if (row.preset === preset) return row;
  }
  return null;
}

function main() {
  const opts = parseArgs(process.argv.slice(2));
  git(["fetch", "-q", "origin"]);
  const sha = git(["rev-parse", opts.ref]);
  if (!existsSync(opts.worktree)) git(["worktree", "add", "--detach", opts.worktree, sha]);
  git(["checkout", "-q", "--detach", sha], opts.worktree);
  console.log(`perf-track: ${opts.ref} = ${sha.slice(0, 7)} in ${opts.worktree}`);

  sh(`source ~/emsdk/emsdk_env.sh >/dev/null 2>&1 && cmake --preset ${opts.preset} --fresh >/dev/null && cmake --build --preset ${opts.preset} 2>&1 | tail -1`,
     opts.worktree);
  const webDir = join(opts.worktree, "platform/web");
  if (!existsSync(join(webDir, "node_modules"))) sh("npx --yes pnpm install --frozen-lockfile 2>&1 | tail -1", webDir);

  const out = join(tmpdir(), `perf-track-${sha.slice(0, 7)}-${Date.now()}`);
  mkdirSync(out, { recursive: true });
  const title = runPerf(opts, webDir, ["--warmup-slices", "860000", "--seconds", "30"], join(out, "title.json"));
  const play = runPerf(opts, webDir, ["--no-build", "--phases", PHASES, ...PRESSES, "--seconds", "30",
                                      "--shot", join(out, "gameplay.png")], join(out, "gameplay.json"));

  const sum = (key) => play.phases.reduce((total, phase) => total + phase[key], 0);
  const loadCpu = sum("cpuS"), loadVirtual = sum("virtualS");
  const row = {
    commit: sha.slice(0, 10),
    date: new Date().toISOString().slice(0, 16),
    preset: opts.preset + (opts.urlParams ? `?${opts.urlParams}` : ""),
    backend: play.backend ?? "?",
    host_cores: String(play.hostCores ?? "?"),
    load_avg_start: title.loadAvgStart.toFixed(1),
    load_avg_end: play.loadAvgEnd.toFixed(1),
    worker_s_to_title: (play.phases[0]?.workerS ?? 0).toFixed(1),
    worker_s_to_gameplay: sum("workerS").toFixed(1),
    cpu_s_to_gameplay: loadCpu.toFixed(1),
    virtual_s_to_gameplay: loadVirtual.toFixed(1),
    title_fps: title.window.fps.toFixed(2),
    title_slices_per_s: title.window.slicesPerS.toFixed(0),
    title_ticks_per_s: title.window.ticksPerS.toFixed(0),
    gameplay_fps: play.window.fps.toFixed(2),
    gameplay_slices_per_s: play.window.slicesPerS.toFixed(0),
    gameplay_ticks_per_s: play.window.ticksPerS.toFixed(0),
    jit_modules_to_gameplay: String(sum("modules")),
    load_cpu_per_vs: (loadCpu / loadVirtual).toFixed(3),
    title_cpu_per_vs: (title.window.cpuS / title.window.virtualS).toFixed(3),
    gameplay_cpu_per_vs: (play.window.cpuS / play.window.virtualS).toFixed(3),
  };

  const before = previousRow(opts.csv, row.preset);
  mkdirSync(dirname(opts.csv), { recursive: true });
  if (!existsSync(opts.csv)) appendFileSync(opts.csv, COLUMNS.join(",") + "\n");
  appendFileSync(opts.csv, COLUMNS.map((c) => row[c]).join(",") + "\n");
  console.log(`\nperf-track row: ${COLUMNS.map((c) => `${c}=${row[c]}`).join(" ")}`);

  let regressed = false;
  if (before) {
    for (const metric of NORMALISED) {
      const was = Number(before[metric]), now = Number(row[metric]);
      if (!(was > 0)) continue;
      const change = (now - was) / was;
      const line = `${metric} ${was} -> ${now} (${change >= 0 ? "+" : ""}${(100 * change).toFixed(1)}%) vs ${before.commit}`;
      if (change > opts.threshold) {
        regressed = true;
        console.log(`REGRESSION ${line}`);
      } else {
        console.log(`compare ${line}`);
      }
    }
  }
  console.log(`TRACKED ${sha.slice(0, 10)} ${regressed ? "regression" : "ok"}`);
}

main();
