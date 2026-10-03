# Instructions for bot 1, bot 2, bot 3, bot 4

If you were told "you are bot N, read instructions for your name", read
this file in full, then `docs/handoff/BOT_N.md` for your own task.

## The project and the goal

Voland is a Nintendo Switch emulator that runs in the browser (C11 core
compiled to WASM, WebGPU, TypeScript web shell). The owner's goal: their
legally owned, self-decrypted Hollow Knight: Silksong runs **playably** in
the browser, at 15 fps at least and 30 fps ideally. Today it boots, plays
through menus into gameplay, and renders correctly, at about **3 fps** in
the browser.

Six Claude Code agents work on this in parallel:

| Agent | Work | Where |
|---|---|---|
| Renderer agent (coordinator) | WebGPU renderer, merging, measurement | `local/dev` in `~/WORKSPACE/Voland` |
| JIT agent | ARM64 → WebAssembly JIT | `local/jit` in `~/WORKSPACE/Voland-jit` (`docs/handoff/JIT_AGENT.md`) |
| bot 1 | Parallel guest threads | see `docs/handoff/BOT_1.md` |
| bot 2 | Video playback (cutscenes play black) | see `docs/handoff/BOT_2.md` |
| bot 3 | WebGPU renderer quality and coverage | see `docs/handoff/BOT_3.md` |
| bot 4 | Browser-side performance (build, worker, wasm) | see `docs/handoff/BOT_4.md` |

## Rules (all bots; non-negotiable)

1. **Read `CLAUDE.md`.** Hard rules 1–10 stand.
   - No decryption code and no key handling.
   - Never copy code from other emulators (reading them for behaviour is
     fine).
   - No Nintendo servers.
   - Don't commit third-party binaries.
   - `core/cpu/backends/ballistic/**` and `recompiler/` are off-limits.
   - The owner has waived the *process* rules: no header-review stops, no
     PRs, no phase gating.
2. **Your own worktree, branch and status file.** Never edit files in
   `~/WORKSPACE/Voland` itself: that is the coordinator's checkout and it
   has uncommitted work. Start with:
   ```
   cd ~/WORKSPACE/Voland && git worktree add ../Voland-botN -b local/botN local/dev
   cd ../Voland-botN
   ```
   (N = your number.) Commit there, then `git push origin local/botN`.
   **Never push `local/dev`, `local/jit` or another bot's branch.** The
   repo `helloimthesigma-blip/Voland` is private: keep it private and never push
   to `upstream`.
   Keep `docs/handoff/BOT_N_STATUS.md` current in your branch: what works,
   measurements, what's next, what you need from others. The coordinator
   merges your branch when a milestone is done.
3. **Stay in your lane.** Each BOT_N.md lists the files you own and the
   files you may touch lightly. If you need a change in someone else's
   area, write it under "Needs" in your status file instead of making it.
4. **Share the machine.** It is one 8-core Mac with six agents on it.
   - Run at most **one** long emulator run (more than 5 minutes) at a time.
   - Launch long runs with `nohup ... &` (tool background jobs die at 2 h).
   - The disk is nearly full (about 20 GB free). Delete big recordings and
     dumps when done, never write more than 3 GB, and check `df -h` before
     big runs.
   - Each worktree needs its own build: `cmake --preset native-noop` etc.,
     and `npm ci` in `platform/web` if you touch web code. Build
     directories are per-worktree.
5. **Prove it.** "It should work" isn't a status. Before each milestone
   commit:
   - `ctest --preset native-noop` passes.
   - For web changes, `cd platform/web && npm run typecheck && npx
     playwright test --project=chromium` passes.
   - Report measured numbers in your status file.
6. **Don't ask the owner questions** unless you're truly blocked. They are
   away. Decide sensibly and write the decision down.

## Facts you will need

- **Silksong NCA:**
  `~/games/game.nca`
- **Native run to gameplay** (deterministic, about 45 min in software
  mode, about 10 in GPU mode):
  ```
  build/native-release/platform/cli/voland-cli run "$NCA" --backend interpreter \
    --input 860000:1:3000 --input 940000:1:3000 --input 1020000:1:3000 \
    --input 3000000:1:3000 --input 3100000:1:3000 --input 3200000:1:3000
  ```
  The title screen is at about 860k slices and gameplay starts at about
  4.7M.
  - In zsh, use an array or `${=VAR}` when the flags come from a variable.
  - Add `--gpu-stream FILE` (or `/dev/null`) to run the WebGPU renderer's
    producer, which is much faster natively than software rasterising.
  - `--dump-frame F.ppm [--dump-frames-every N]` writes frames.
  - `VOLAND_PC_PROFILE=1` prints where guest cycles go.
- **Builds:**
  - Native: `cmake --preset native-noop|native-release && cmake --build --preset ...`.
  - Web: `source ~/emsdk/emsdk_env.sh && cmake --preset web && cmake --build --preset web`.
    This stages the core into `platform/web/public/core`.
- **Browser on the real GPU:**
  ```
  cd platform/web && VOLAND_HOMEBREW_NRO="$NCA" VOLAND_HOMEBREW_RUN_MS=240000 \
    VOLAND_HOMEBREW_FPS_MS=60000 npx playwright test --project=chromium-gpu e2e/homebrew.spec.ts -g "boots"
  ```
  It prints guest fps samples. Headless Chromium falls back to SwiftShader
  unless `--use-angle=metal`, which the `chromium-gpu` project passes.
- **Design docs:** `docs/DESIGN.md` (v3.71.0; the changelog at the end
  describes recent work), `docs/GPU_COMMAND_STREAM.md`,
  `docs/handoff/RENDERER_STATUS.md`.
- **Profile so far** (to the title screen): guest main thread 53% of
  cycles, guest thread 32 at 36%. Host time is almost all ARM
  interpretation. GPU time is about 4.4 ms per frame.
