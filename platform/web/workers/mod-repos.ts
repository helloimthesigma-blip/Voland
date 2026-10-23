/**
 * Mod repositories (docs/DESIGN.md §19, "add-a-repo model"). The user adds
 * a repository by URL; its repository.json lists mods per title, each a
 * CORS-reachable archive with a mandatory SHA-256. Voland ships no
 * repositories, lists none, and never proxies a fetch: a repository that
 * is not CORS-enabled is reported as the repository's problem. Only
 * guest-side mod types exist ("romfs", "exefs-patch", "cheats",
 * "guest-nro"); there is no type for code that runs in the page.
 *
 * Install: fetch the archive, verify its SHA-256 before anything is
 * unpacked, unpack it into the title's mods (workers/mod-store.ts) and
 * record where it came from so updates can be offered. Executable patches
 * and cheats carry the NSO build id they were made for; a mismatch with
 * the game's build is shown and the entry is not installable.
 */
import { installMod, recordModSource, safeModName, zipFiles } from "./mod-store.ts";

export const MANIFEST_FILE = "repository.json";
const SUPPORTED_SCHEMAS = new Set([1, 2]);
const MOD_TYPES = new Set(["romfs", "exefs-patch", "cheats", "guest-nro"]);
const SHA256_HEX = /^[0-9a-f]{64}$/i;
const TITLE_ID = /^[0-9a-f]{16}$/i;

export interface RepoEntry {
  readonly titleId: string;
  readonly modId: string;
  readonly name: string;
  readonly version: string;
  readonly type: string;
  readonly targetGameVersions: readonly string[];
  readonly buildId: string | null;
  readonly artifactUrl: string;
  readonly sha256: string;
  readonly sizeBytes: number;
}

export interface RepoManifest {
  readonly url: string; /* where it was fetched from */
  readonly name: string;
  readonly entries: readonly RepoEntry[];
  /** Entries dropped as malformed (shown, not hidden). */
  readonly rejected: number;
}

/** The manifest URL for what the user typed: a repository.json URL as is,
 * otherwise the repository root + repository.json. https only (localhost
 * is allowed for authors testing a repo). Null when not a usable URL. Pure. */
export function manifestUrl(input: string): string | null {
  let url: URL;
  try {
    url = new URL(input.trim());
  } catch {
    return null;
  }
  const local = url.hostname === "localhost" || url.hostname === "127.0.0.1";
  if (url.protocol !== "https:" && !(local && url.protocol === "http:")) return null;
  if (!url.pathname.endsWith(".json")) url.pathname = `${url.pathname.replace(/\/+$/, "")}/${MANIFEST_FILE}`;
  return url.href;
}

function str(v: unknown): string | null {
  return typeof v === "string" && v.length > 0 ? v : null;
}

/** Validates a parsed repository.json. Malformed entries are dropped and
 * counted; a manifest without the basics throws. `servers` (schema 2) are
 * ignored: no server directory exists in this build. Pure. */
export function parseManifest(url: string, json: unknown): RepoManifest {
  if (typeof json !== "object" || json === null) throw new Error("repository.json is not a JSON object");
  const m = json as Record<string, unknown>;
  if (typeof m["schemaVersion"] !== "number" || !SUPPORTED_SCHEMAS.has(m["schemaVersion"])) {
    throw new Error(`unsupported schemaVersion ${String(m["schemaVersion"])}`);
  }
  if (!Array.isArray(m["entries"])) throw new Error("repository.json has no entries list");
  const entries: RepoEntry[] = [];
  let rejected = 0;
  for (const raw of m["entries"] as unknown[]) {
    const e = (typeof raw === "object" && raw !== null ? raw : {}) as Record<string, unknown>;
    const titleId = str(e["titleId"]);
    const type = str(e["type"]);
    const sha256 = str(e["sha256"]);
    const artifactUrl = str(e["artifactUrl"]);
    const buildId = str(e["buildId"]);
    let artifact: URL | null = null;
    try {
      artifact = artifactUrl ? new URL(artifactUrl, url) : null;
    } catch {
      artifact = null;
    }
    const ok = titleId && TITLE_ID.test(titleId) && str(e["modId"]) && str(e["name"]) && str(e["version"]) &&
      type && MOD_TYPES.has(type) && sha256 && SHA256_HEX.test(sha256) && artifact &&
      (artifact.protocol === "https:" || artifact.hostname === "localhost") &&
      typeof e["sizeBytes"] === "number" &&
      /* Executable patches and cheats are matched by build id (§19). */
      (buildId !== null || (type !== "exefs-patch" && type !== "cheats"));
    if (!ok || !artifact || !titleId || !type || !sha256) {
      rejected++;
      continue;
    }
    entries.push({
      titleId: titleId.toLowerCase(),
      modId: String(e["modId"]),
      name: String(e["name"]),
      version: String(e["version"]),
      type,
      targetGameVersions: Array.isArray(e["targetGameVersions"]) ? (e["targetGameVersions"] as unknown[]).map(String) : [],
      buildId: buildId ? buildId.toLowerCase() : null,
      artifactUrl: artifact.href,
      sha256: sha256.toLowerCase(),
      sizeBytes: e["sizeBytes"] as number,
    });
  }
  return { url, name: str(m["name"]) ?? new URL(url).host, entries, rejected };
}

/** Fetches and validates a repository's manifest. Errors explain CORS
 * failures in the user's terms. */
export async function fetchManifest(input: string): Promise<RepoManifest> {
  const url = manifestUrl(input);
  if (!url) throw new Error("not an https URL");
  let response: Response;
  try {
    response = await fetch(url, { mode: "cors", credentials: "omit", cache: "no-cache" });
  } catch {
    throw new Error(`could not reach ${url} - the repository must allow cross-origin requests (CORS); that is the repository's setting to fix`);
  }
  if (!response.ok) throw new Error(`${url}: HTTP ${response.status}`);
  return parseManifest(url, await response.json());
}

/** Compares dotted versions ("1.10.0" > "1.9.2"); non-numeric parts
 * compare as text. Pure. */
export function compareVersions(a: string, b: string): number {
  const pa = a.replace(/^v/i, "").split(/[.\-+]/);
  const pb = b.replace(/^v/i, "").split(/[.\-+]/);
  for (let i = 0; i < Math.max(pa.length, pb.length); i++) {
    const x = pa[i] ?? "0";
    const y = pb[i] ?? "0";
    const nx = Number(x);
    const ny = Number(y);
    const c = Number.isFinite(nx) && Number.isFinite(ny) ? nx - ny : x.localeCompare(y);
    if (c !== 0) return c < 0 ? -1 : 1;
  }
  return 0;
}

/** Why an entry cannot be installed for the running build, or null.
 * `mainBuildId` is the game's main NSO build id (hex) when known. Pure. */
export function blockedReason(entry: RepoEntry, mainBuildId: string | null): string | null {
  if (!entry.buildId) return null;
  if (!mainBuildId) return "load the game once in this session to check that this patch matches your game version";
  const want = entry.buildId.replace(/0+$/, "");
  return mainBuildId.toLowerCase().startsWith(want) ? null : "made for a different game version (build id does not match)";
}

function hex(bytes: ArrayBuffer): string {
  return Array.from(new Uint8Array(bytes), (b) => b.toString(16).padStart(2, "0")).join("");
}

/** Downloads, verifies and installs one entry. Returns the files written. */
export async function installEntry(manifest: RepoManifest, entry: RepoEntry,
                                   onProgress?: (text: string) => void): Promise<number> {
  onProgress?.(`Downloading ${entry.name}...`);
  let response: Response;
  try {
    response = await fetch(entry.artifactUrl, { mode: "cors", credentials: "omit" });
  } catch {
    throw new Error(`could not download ${entry.artifactUrl} - its host must allow cross-origin requests (CORS)`);
  }
  if (!response.ok) throw new Error(`${entry.artifactUrl}: HTTP ${response.status}`);
  const bytes = await response.arrayBuffer();
  onProgress?.(`Checking ${entry.name}...`);
  const digest = hex(await crypto.subtle.digest("SHA-256", bytes));
  if (digest !== entry.sha256) {
    throw new Error(`${entry.name}: the download does not match the repository's SHA-256 - not installed`);
  }
  const archive = new Blob([bytes]);
  const head = new Uint8Array(bytes, 0, Math.min(4, bytes.byteLength));
  if (!(head[0] === 0x50 && head[1] === 0x4b)) {
    throw new Error(`${entry.name}: only .zip archives can be unpacked in the browser (re-pack 7z/rar as zip)`);
  }
  const modName = safeModName(entry.name);
  const written = await installMod(entry.titleId, modName, zipFiles(archive),
                                   (n) => onProgress?.(`Installing ${entry.name}: ${n} file(s)...`));
  await recordModSource(entry.titleId, modName, { repo: manifest.url, modId: entry.modId, version: entry.version });
  return written;
}

/* ---- the repository list (IndexedDB settings, §19) ------------------ */

const DB_NAME = "voland-mod-repos";
const STORE = "repos";

function openDb(): Promise<IDBDatabase | null> {
  if (typeof indexedDB === "undefined") return Promise.resolve(null);
  return new Promise((resolve) => {
    const request = indexedDB.open(DB_NAME, 1);
    request.onupgradeneeded = (): void => {
      if (!request.result.objectStoreNames.contains(STORE)) request.result.createObjectStore(STORE, { keyPath: "url" });
    };
    request.onsuccess = (): void => resolve(request.result);
    request.onerror = (): void => resolve(null);
  });
}

function run<T>(mode: IDBTransactionMode, body: (store: IDBObjectStore) => IDBRequest<T>): Promise<T | null> {
  return openDb().then((db) => new Promise<T | null>((resolve) => {
    if (!db) {
      resolve(null);
      return;
    }
    const request = body(db.transaction(STORE, mode).objectStore(STORE));
    request.onsuccess = (): void => resolve(request.result);
    request.onerror = (): void => resolve(null);
  }));
}

export interface SavedRepo {
  readonly url: string;
  readonly name: string;
  readonly addedAt: number;
}

export async function listRepos(): Promise<readonly SavedRepo[]> {
  const all = await run<unknown[]>("readonly", (s) => s.getAll());
  return (all ?? []).filter((r): r is SavedRepo => typeof (r as SavedRepo).url === "string")
    .sort((a, b) => a.addedAt - b.addedAt);
}

/** Adding a repository connects to nothing else and changes no installed
 * mod. */
export async function addRepo(manifest: RepoManifest): Promise<void> {
  await run("readwrite", (s) => s.put({ url: manifest.url, name: manifest.name, addedAt: Date.now() } satisfies SavedRepo));
}

/** Removing a repository leaves its installed mods alone. */
export async function removeRepo(url: string): Promise<void> {
  await run("readwrite", (s) => s.delete(url));
}
