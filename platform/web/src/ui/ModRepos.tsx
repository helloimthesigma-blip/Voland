/**
 * Mod repositories (§19) inside the Mods panel: add a repository by URL
 * (or an "Add to Voland" link: ?add-repo=<url>), see what it offers for
 * the chosen game, install or update with the archive's SHA-256 checked
 * first. Voland lists no repositories of its own. Adding one is refused
 * inside a frame from another site (DESIGN.md §16: consent is never given
 * through someone else's page).
 */
import { For, Show, createSignal, onMount } from "solid-js";
import {
  type RepoManifest, type SavedRepo, addRepo, blockedReason, compareVersions, fetchManifest, installEntry, listRepos,
  removeRepo,
} from "@workers/mod-repos";
import type { ModInfo } from "@workers/mod-store";

interface ModReposProps {
  readonly titleId: string;
  readonly buildId: string | null;
  readonly installed: readonly ModInfo[];
  readonly onInstalled: () => void;
}

function crossOriginFramed(): boolean {
  try {
    return window.top !== window.self && window.top?.location.origin !== window.location.origin;
  } catch {
    return true;
  }
}

function formatBytes(bytes: number): string {
  return bytes >= 1024 ** 3 ? `${(bytes / 1024 ** 3).toFixed(1)} GB` : `${Math.max(1, Math.round(bytes / 1024 ** 2))} MB`;
}

const ADD_WARNING =
  "Voland does not check what a repository contains. Mods from it run inside the emulated console only, " +
  "but they can change or break your game and its saves. Only add repositories you trust.";

function ModRepos(props: ModReposProps) {
  const [repos, setRepos] = createSignal<readonly SavedRepo[]>([]);
  const [manifests, setManifests] = createSignal<readonly RepoManifest[]>([]);
  const [errors, setErrors] = createSignal<readonly string[]>([]);
  const [url, setUrl] = createSignal("");
  const [note, setNote] = createSignal<string | null>(null);
  const [busy, setBusy] = createSignal(false);
  const framed = crossOriginFramed();

  const refresh = async (): Promise<void> => {
    const list = await listRepos();
    setRepos(list);
    const found: RepoManifest[] = [];
    const failed: string[] = [];
    for (const repo of list) {
      try {
        found.push(await fetchManifest(repo.url));
      } catch (e) {
        failed.push(`${repo.name}: ${e instanceof Error ? e.message : String(e)}`);
      }
    }
    setManifests(found);
    setErrors(failed);
  };

  const add = async (input: string): Promise<void> => {
    if (framed) return;
    setBusy(true);
    try {
      const manifest = await fetchManifest(input);
      if (!confirm(`Add the mod repository "${manifest.name}" (${manifest.url})?\n\n${ADD_WARNING}`)) return;
      await addRepo(manifest);
      setUrl("");
      setNote(`Added ${manifest.name}: ${manifest.entries.length} mod(s)` +
              (manifest.rejected ? `, ${manifest.rejected} malformed entr${manifest.rejected === 1 ? "y" : "ies"} skipped` : "") + ".");
      await refresh();
    } catch (e) {
      setNote(`Could not add the repository: ${e instanceof Error ? e.message : String(e)}`);
    } finally {
      setBusy(false);
    }
  };

  onMount(() => {
    void refresh();
    const requested = new URLSearchParams(window.location.search).get("add-repo");
    if (requested && !framed) void add(requested);
  });

  const offers = () => manifests().flatMap((m) => m.entries.filter((e) => e.titleId === props.titleId.toLowerCase())
    .map((e) => ({ manifest: m, entry: e })));

  const installedFrom = (repo: string, modId: string): ModInfo | undefined =>
    props.installed.find((m) => m.source?.repo === repo && m.source.modId === modId);

  return (
    <details class="voland-mod-repos" data-testid="mod-repos">
      <summary>Mod repositories</summary>
      <Show when={!framed} fallback={<p class="voland-load-note">Open Voland directly (not inside another site) to add repositories.</p>}>
        <div class="voland-setting">
          <input type="url" placeholder="https://.../repository.json" value={url()} data-testid="repo-url"
                 onInput={(e) => setUrl(e.currentTarget.value)} />
          <button type="button" class="voland-load-button voland-load-secondary" disabled={busy() || !url()}
                  onClick={() => void add(url())}>Add</button>
        </div>
        <small>Voland comes with no repositories. A repository is a repository.json file on a site that allows cross-origin requests.</small>
      </Show>
      <Show when={note()}>{(text) => <p class="voland-load-note">{text()}</p>}</Show>
      <For each={errors()}>{(text) => <p class="voland-load-note">{text}</p>}</For>
      <ul class="voland-mod-list">
        <For each={repos()}>
          {(repo) => (
            <li class="voland-mod">
              <span class="voland-mod-name">{repo.name}</span>
              <span class="voland-mod-meta">{repo.url}</span>
              <span class="voland-mod-actions">
                <button type="button" title="Remove this repository (installed mods stay)"
                        onClick={() => void removeRepo(repo.url).then(refresh)}>×</button>
              </span>
            </li>
          )}
        </For>
      </ul>
      <Show when={offers().length > 0}>
        <h5>Available for this game</h5>
        <ul class="voland-mod-list">
          <For each={offers()}>
            {(offer) => {
              const have = () => installedFrom(offer.manifest.url, offer.entry.modId);
              const blocked = () => blockedReason(offer.entry, props.buildId);
              const action = () => {
                const h = have();
                if (!h) return "Install";
                return h.source && compareVersions(offer.entry.version, h.source.version) > 0 ? `Update to ${offer.entry.version}` : null;
              };
              return (
                <li class="voland-mod">
                  <span class="voland-mod-name">{offer.entry.name} {offer.entry.version}</span>
                  <span class="voland-mod-meta">
                    {offer.entry.type} · {formatBytes(offer.entry.sizeBytes)} · from {offer.manifest.name}
                    <Show when={offer.entry.targetGameVersions.length > 0}> · for game version {offer.entry.targetGameVersions.join(", ")}</Show>
                  </span>
                  <Show when={blocked()}>{(why) => <span class="voland-mod-meta">{why()}</span>}</Show>
                  <Show when={!blocked() && action()}>
                    {(label) => (
                      <button type="button" class="voland-load-button voland-load-secondary" disabled={busy()}
                              onClick={() => {
                                setBusy(true);
                                installEntry(offer.manifest, offer.entry, (t) => setNote(t))
                                  .then((n) => setNote(`Installed ${offer.entry.name} (${n} file(s)). It applies the next time you load the game.`))
                                  .catch((e: unknown) => setNote(e instanceof Error ? e.message : String(e)))
                                  .finally(() => { setBusy(false); props.onInstalled(); });
                              }}>{label()}</button>
                    )}
                  </Show>
                  <Show when={have() && !action()}><span class="voland-mod-meta">installed</span></Show>
                </li>
              );
            }}
          </For>
        </ul>
      </Show>
    </details>
  );
}

export default ModRepos;
