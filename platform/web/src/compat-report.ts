/**
 * The compatibility report: one text a player can copy when a game does
 * not work, holding what is needed to see why - the core's distinct
 * warnings/errors with counts (unimplemented services and commands,
 * failing calls, GPU problems), renderer counters, the run state and speed,
 * and the browser/GPU it ran on. The core half comes from the CPU worker
 * (compat-report message); formatting is pure.
 */
import type { CPUToMainMessage, CoreReport, MainToCPUMessage } from "@bindings/protocol";

export interface ReportContext {
  readonly gameName: string;
  readonly runState: string;
  readonly runDetail: string;
  readonly fps: number;
  readonly gpuAdapter: string;
  readonly userAgent: string;
  readonly settings: string;
  readonly now: Date;
}

let worker: Worker | null = null;
const pending: ((report: CoreReport) => void)[] = [];

export function registerReportWorker(cpuWorker: Worker): void {
  worker = cpuWorker;
}

export function handleReportMessage(msg: CPUToMainMessage): boolean {
  if (msg.type !== "compat-report") return false;
  pending.shift()?.(msg.report);
  return true;
}

export function requestCoreReport(): Promise<CoreReport | null> {
  const target = worker;
  if (!target) return Promise.resolve(null);
  return new Promise((resolve) => {
    pending.push(resolve);
    target.postMessage({ type: "compat-report" } satisfies MainToCPUMessage);
  });
}

/** Groups problem lines by kind so the report reads by area. Pure. */
export function problemArea(line: string): string {
  if (/unimplemented command|no HLE service|unimplemented control/.test(line)) return "Missing services / commands";
  if (/\[ipc\].*failed/.test(line)) return "Calls that failed";
  if (/svcBreak|fault|crash|undefined instruction/i.test(line)) return "Crashes and aborts";
  if (/\[gpu\]|\[nvdrv\]|texture|shader|wgsl/i.test(line)) return "Graphics";
  if (/\[aud|audio/i.test(line)) return "Audio";
  return "Other";
}

const AREA_ORDER = ["Crashes and aborts", "Missing services / commands", "Calls that failed", "Graphics", "Audio", "Other"];

/** The report text. Pure. */
export function formatReport(core: CoreReport | null, context: ReportContext): string {
  const lines: string[] = [];
  lines.push(`Voland compatibility report - ${context.now.toISOString()}`);
  lines.push(`Game: ${context.gameName}${core?.titleId ? ` (${core.titleId})` : ""}`);
  lines.push(`State: ${context.runState}${context.runDetail ? ` - ${context.runDetail}` : ""}`);
  if (core) {
    const speed = core.wallSeconds > 0 ? core.virtualSeconds / core.wallSeconds : 0;
    lines.push(`Ran: ${core.virtualSeconds.toFixed(1)} s of game time in ${core.wallSeconds.toFixed(1)} s (${speed.toFixed(2)}x), ${core.slices} slices, ${context.fps} fps now`);
    lines.push(`CPU backend: ${core.backend}`);
  }
  lines.push(`GPU: ${context.gpuAdapter}`);
  lines.push(`Browser: ${context.userAgent}`);
  if (context.settings) lines.push(`Settings: ${context.settings}`);
  if (core) {
    lines.push("");
    lines.push("Renderer: " + Object.entries(core.render).map(([k, v]) => `${k}=${v}`).join(" "));
    const byArea = new Map<string, string[]>();
    for (const raw of core.problems.split("\n")) {
      if (!raw.trim()) continue;
      const [count = "", ...rest] = raw.split("\t");
      const text = rest.join("\t").replace(/^\[(WARN |ERROR)\] /, "");
      const area = problemArea(text);
      byArea.set(area, [...(byArea.get(area) ?? []), `  ${count.padStart(7)}x  ${text}`]);
    }
    for (const area of AREA_ORDER) {
      const entries = byArea.get(area);
      if (!entries) continue;
      lines.push("");
      lines.push(`${area}:`);
      lines.push(...entries);
    }
    if (byArea.size === 0) {
      lines.push("");
      lines.push("No warnings or errors from the emulator core.");
    }
  }
  return `${lines.join("\n")}\n`;
}
