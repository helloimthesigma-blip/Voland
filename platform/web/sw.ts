/**
 * Service Worker (docs/DESIGN.md section 16). Two jobs:
 *
 * 1. Re-inject COOP/COEP/CSP on every served response. Real deployments
 *    should set these at the server/hosting-config level; the SW path is
 *    the fallback for self-hosters who can't - and requires one reload on
 *    first visit before it can control the page (handled in main.ts).
 * 2. Cache strategy: network-first for everything same-origin (the cache is
 *    the offline fallback), `/api/` network-only. Navigations resolve to the
 *    shell so client-side routing survives a hard refresh at e.g.
 *    /game/:titleId. The core's `.wasm` is NOT cache-first: its URL is not
 *    content-hashed, so a cached copy outlives updates while the matching
 *    switch_core.js does not - a stale wasm under a new loader fails at init
 *    (missing exports). Activation deletes every cache this version does not
 *    use, purging such stale copies.
 *
 * Phase 0 has no build-time precache manifest (that needs a bundler
 * plugin, e.g. vite-plugin-pwa, which is not wired up yet) - caches are
 * filled lazily as requests are served rather than precached at install.
 */

const sw: ServiceWorkerGlobalScope = globalThis as unknown as ServiceWorkerGlobalScope;

const SHELL_CACHE = "voland-shell-v2";
const WASM_CACHE = "voland-wasm-v2";
const CURRENT_CACHES: ReadonlySet<string> = new Set([SHELL_CACHE, WASM_CACHE]);
const SHELL_URL = "/index.html";

/* COOP must be strict "same-origin" - "same-origin-allow-popups" never
 * yields crossOriginIsolated in shipping Chromium regardless of COEP (see
 * vite.config.ts for the full explanation and docs/DESIGN.md §16). */
const CROSS_ORIGIN_ISOLATION_HEADERS: ReadonlyArray<readonly [string, string]> = [
  ["Cross-Origin-Opener-Policy", "same-origin"],
  ["Cross-Origin-Embedder-Policy", "require-corp"],
  ["Content-Security-Policy", "frame-ancestors 'none'"],
];

function addCrossOriginIsolationHeaders(response: Response): Response {
  const headers = new Headers(response.headers);
  for (const [key, value] of CROSS_ORIGIN_ISOLATION_HEADERS) {
    headers.set(key, value);
  }
  return new Response(response.body, {
    status: response.status,
    statusText: response.statusText,
    headers,
  });
}

/* Network first; the cached copy only when the network fails (offline). */
async function networkFirst(request: Request, cacheName: string, key: Request = request): Promise<Response> {
  const cache = await sw.caches.open(cacheName);
  try {
    const response = await fetch(request);
    if (response.ok) await cache.put(key, response.clone());
    return addCrossOriginIsolationHeaders(response);
  } catch (e) {
    const cached = await cache.match(key);
    if (cached) return addCrossOriginIsolationHeaders(cached);
    throw e;
  }
}

sw.addEventListener("install", () => {
  sw.skipWaiting();
});

sw.addEventListener("activate", (event: ExtendableEvent) => {
  event.waitUntil((async (): Promise<void> => {
    const names = await sw.caches.keys();
    await Promise.all(names.filter((name) => !CURRENT_CACHES.has(name)).map((name) => sw.caches.delete(name)));
    await sw.clients.claim();
  })());
});

sw.addEventListener("fetch", (event: FetchEvent) => {
  const url = new URL(event.request.url);
  if (url.origin !== location.origin) return; // let cross-origin requests through untouched

  if (url.pathname.startsWith("/api/")) {
    event.respondWith(fetch(event.request).then(addCrossOriginIsolationHeaders));
    return;
  }

  if (url.pathname.endsWith(".wasm")) {
    event.respondWith(networkFirst(event.request, WASM_CACHE));
    return;
  }

  if (event.request.mode === "navigate") {
    // Navigation API + client-side routing (§16): every navigate request
    // resolves to the cached shell so a hard refresh at any route works.
    event.respondWith(networkFirst(event.request, SHELL_CACHE, new Request(SHELL_URL)));
    return;
  }

  event.respondWith(networkFirst(event.request, SHELL_CACHE));
});
