/**
 * The page's event log, made readable. The core's event feed (common/log.h
 * log_take_events) carries milestones ("NOTE ...") and each distinct
 * warning or error the first time it happens; this turns those raw lines
 * into short plain-language entries, folds families of similar problems
 * (one line per untranslated shader...) into one explained entry plus a
 * periodic "N more" count, and writes a status line every so often while
 * a game runs. Pure functions plus a small stateful folder; the CPU worker
 * owns the timing.
 */
/** The levels the event log shows. */
export type FeedLevel = "info" | "warn" | "error";

export interface FeedEntry {
  readonly level: FeedLevel;
  readonly message: string;
}

interface Rule {
  readonly match: RegExp;
  /** Problems that share a family are told once, then counted. */
  readonly family: (m: RegExpExecArray) => string;
  readonly level: FeedLevel;
  readonly explain: (m: RegExpExecArray) => string;
  /** How a later member is counted in the periodic summary. */
  readonly more?: string;
}

const RULES: readonly Rule[] = [
  {
    match: /\[gpu\] (pixel|vertex|compute|geometry|tess\w*) program [0-9a-f]+ not translated: (.+)$/i,
    family: (m) => `untranslated:${m[2]}`,
    level: "warn",
    explain: (m) => `graphics: a ${m[1]} shader uses ${m[2]}, which Voland cannot run on the GPU yet - its draws are skipped or drawn on the CPU (slower, may look wrong)`,
    more: "more shaders could not be translated",
  },
  {
    match: /\[gpu\] vertices on the CPU: (.+)$/i,
    family: () => "cpu-vertices",
    level: "info",
    explain: () => "graphics: some vertex work runs on the CPU instead of the GPU (slower)",
    more: "more draws did vertex work on the CPU",
  },
  {
    match: /\[gpu\] texture (\d+)x(\d+) format (0x[0-9a-f]+).*larger than the texture cache/i,
    family: () => "texture-too-large",
    level: "warn",
    explain: (m) => `graphics: a ${m[1]}x${m[2]} texture (format ${m[3]}) is larger than the texture cache - it is not shown`,
    more: "more textures were too large",
  },
  {
    match: /\[gpu\] draw skipped: (.+)$/i,
    family: () => "draw-skipped",
    level: "info",
    explain: () => "graphics: a draw was skipped (its target is set up in a way Voland does not handle yet)",
    more: "more kinds of draws were skipped",
  },
  {
    match: /\[ipc\] (\w+):(\w+) failed: 0x202$/,
    family: () => "file-not-found",
    level: "info",
    explain: () => "the game looked for a file that does not exist (usually normal: games check for optional files)",
    more: "more missing files were looked up",
  },
  {
    match: /\[ipc\] (\w+):(\w+) failed: (0x[0-9a-f]+)$/i,
    family: (m) => `ipc:${m[1]}:${m[2]}`,
    level: "warn",
    explain: (m) => `the game asked the system for ${m[1]}.${m[2]} and Voland answered with error ${m[3]}`,
  },
  {
    match: /unknown command (\d+) on (\S+)|(\S+): unimplemented command (\d+)/i,
    family: (m) => `unimpl:${m[2] ?? m[3]}`,
    level: "warn",
    explain: (m) => `the game used a system call Voland does not have yet (${m[2] ?? m[3]} command ${m[1] ?? m[4]}); it was answered with defaults`,
    more: "more unimplemented system calls",
  },
];

/** One raw feed line ("NOTE text", "WARN [x] text", "ERROR ...") as a log
 * entry and the family it belongs to (null: always shown). Pure. */
export function explainLine(line: string): { readonly entry: FeedEntry; readonly family: string | null; readonly more: string | null } | null {
  const space = line.indexOf(" ");
  if (space < 0) return null;
  const tag = line.slice(0, space);
  const text = line.slice(space + 1).trim();
  if (text.length === 0) return null;
  if (tag === "NOTE") return { entry: { level: "info", message: text }, family: null, more: null };
  for (const rule of RULES) {
    const m = rule.match.exec(text);
    if (m) return { entry: { level: rule.level, message: rule.explain(m) }, family: rule.family(m), more: rule.more ?? null };
  }
  return { entry: { level: tag === "ERROR" ? "error" : "warn", message: text }, family: null, more: null };
}

/** Folds families: the first of each is shown with its explanation, later
 * ones are counted and reported by `summary()`. */
export class FeedFolder {
  private readonly seen = new Set<string>();
  private readonly pending = new Map<string, { readonly more: string; count: number }>();

  /** Entries to show now for these raw lines. */
  take(lines: readonly string[]): FeedEntry[] {
    const out: FeedEntry[] = [];
    for (const line of lines) {
      const r = explainLine(line);
      if (!r) continue;
      if (r.family === null) {
        out.push(r.entry);
      } else if (!this.seen.has(r.family)) {
        this.seen.add(r.family);
        out.push(r.entry);
      } else if (r.more) {
        const p = this.pending.get(r.more) ?? { more: r.more, count: 0 };
        p.count++;
        this.pending.set(r.more, p);
      }
    }
    return out;
  }

  /** "N more ..." for everything counted since the last summary. */
  summary(): FeedEntry[] {
    const out: FeedEntry[] = [];
    for (const p of this.pending.values()) if (p.count > 0) out.push({ level: "info", message: `${p.count} ${p.more}` });
    this.pending.clear();
    return out;
  }

  reset(): void {
    this.seen.clear();
    this.pending.clear();
  }
}

export interface StatusSample {
  readonly wallSeconds: number;    /* since the game started */
  readonly virtualSeconds: number; /* guest time since the game started */
  readonly frames: number;         /* pictures shown since the game started */
}

function clock(seconds: number): string {
  const s = Math.max(0, Math.floor(seconds));
  return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, "0")}`;
}

/** The periodic status line from two samples (the previous one and now).
 * Pure. */
export function statusLine(before: StatusSample, now: StatusSample): string {
  const wall = now.wallSeconds - before.wallSeconds;
  if (wall <= 0) return `playing for ${clock(now.wallSeconds)}`;
  const fps = (now.frames - before.frames) / wall;
  const speed = (now.virtualSeconds - before.virtualSeconds) / wall;
  const pace = speed >= 0.97 ? "full speed" : `${Math.round(speed * 100)}% speed${speed < 0.75 ? " (slow motion; sound may crackle)" : ""}`;
  return `playing for ${clock(now.wallSeconds)}: ${fps.toFixed(0)} fps, ${pace}`;
}
