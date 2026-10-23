# Mods

Voland applies mods you supply to any game: replaced or added game files
(LayeredFS), executable patches, replacement executables and cheats. It
ships no mods and points to no mod sites (DESIGN.md §19). Everything a mod
contains runs inside the emulated console, never as code in your browser.

## What works

| Kind | Where it goes in a mod folder | What happens |
|---|---|---|
| Game files (LayeredFS) | `romfs/...` | Each file replaces the game's file at the same path, or is added if the game has none. Folders are merged. |
| Executable patches | `exefs/<build id>.ips`, `exefs_patches/<name>/<build id>.ips` (IPS or IPS32), or any `*.pchtxt` | Applied to the matching executable (`main`, `subsdk*`, `sdk`, `rtld`) as it is loaded. The build id in the file name, or `@nsobid-` in a pchtxt, must match the executable, so a patch for another game version does nothing. |
| Replacement executables | `exefs/main`, `exefs/subsdk0`..`subsdk9`, `exefs/sdk`, `exefs/rtld`, `exefs/main.npdm` | The file replaces the game's own, or adds a module the game lacks (for example `subsdk9` plus `main.npdm`, which is how code-injection frameworks load). |
| Cheats | `cheats/<build id>.txt` | Atmosphere cheat format. `{Master}` codes always run, `[Named]` cheats can be switched on and off while playing. They run 12 times a second, as on Atmosphere. |

Common folder layouts work as unpacked:

- `atmosphere/contents/<title id>/romfs/...`, `.../exefs/...`, `.../cheats/...`.
  Files under another game's title id are skipped.
- `<title id>/romfs/...`, or plain `romfs/...`, `exefs/...`, `cheats/...`.
- `atmosphere/exefs_patches/<name>/<build id>.ips`.
- A `.pchtxt` anywhere in the mod.

Readmes, images and other files are ignored.

When two mods change the same file, the one higher in the list wins.
Patches and cheats from all enabled mods apply.

## In the browser

Open **Mods** in the side panel:

1. Load the game once, so Voland knows its title id.
2. Choose the game, then **Add mod folder** or **Add .zip**, or drop mod
   folders and `.zip` files onto the panel. Mods are copied into the
   browser's private storage (OPFS) for that game.
3. Tick or untick mods, and use ▲ / ▼ to set priority.
4. Load the game again. Mod changes apply at the next load.
5. While the game runs, its cheats are listed under **Running now**.
   Cheats switch on and off immediately.

Zip archives can use "stored" or "deflate" compression (what nearly every
zip tool writes), including zip64 for archives over 4 GB. 7z and rar
archives need to be unpacked first, or re-packed as zip.

## On the command line

```
voland-cli run game.nca --mods DIR
voland-cli romfs game.nca [FILTER] [OUT_DIR] --mods DIR   # list / extract the modded RomFS
```

Each subfolder of `DIR` is one mod, applied in name order (earlier names
win). If `DIR` itself contains `romfs/`, `exefs/`, `cheats/` or
`atmosphere/`, it is treated as a single mod.

## Limits

- Mods can only be applied at load. Restart the game after changing them.
- Mods that read their own files from the SD card at run time (for example
  plugin loaders that look in `sd:/...`) need those files on the emulated
  SD card. Add them with **SD card** in the side panel, keeping their
  folder structure.
- Game updates and DLC are not loaded, so mods built for a game update
  only work if their build id matches the base game's executable.
- A LayeredFS mod cannot delete a game file, only replace or add one.

## Mod repositories

You can add a mod repository by URL (DESIGN.md §19): **Mods → Mod
repositories → Add**, or open Voland with `?add-repo=<url>` (the "Add to
Voland" links a repository's site can offer). Voland ships with none.

- A repository is a `repository.json` (schema in DESIGN.md §19) on a site
  that allows cross-origin requests. GitHub Pages and releases do. Voland
  never fetches through a proxy, so a repository without CORS can't be
  used. Its error message says so.
- For the chosen game, the panel lists the repository's mods with their
  version, type and size. **Install** downloads the archive and checks its
  SHA-256 against the repository's before unpacking it (zip only). It
  records where the mod came from, so newer versions show **Update**.
- Executable patches and cheats name the build id they were made for.
  Until the game has been loaded in this session, they are listed as "load
  the game once to check". If the build id doesn't match, they can't be
  installed.
- Adding or removing a repository never touches installed mods. Adding is
  refused when Voland is shown inside another site's frame.
- Only guest-side mod types exist (`romfs`, `exefs-patch`, `cheats`,
  `guest-nro`). There is no type for code that runs in the browser page.

## How it works

The core reads mod files on demand, the same way it reads the game file.
No mod is copied into memory whole.

- **LayeredFS** (`core/hle/loader/mods.c`, `layeredfs_build`) builds a new
  RomFS directory and file table. Unchanged files keep pointing at the
  game's data, and replaced or added files point at the mod's files. The
  game reads the merged image as if it were the original.
- **Patches** are applied to each executable's decompressed image before
  its memory permissions are set.
- **Cheats** (`core/hle/cheat_vm.c`) run on the emulator thread between
  guest runs. They change memory through the softmmu. Writes to code pages
  invalidate the JIT's translations of those pages.
