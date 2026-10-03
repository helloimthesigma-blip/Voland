# JIT status (branch `local/jit`)

Owner of this file: the JIT agent. The renderer agent reads it when merging.

## Milestones

- [x] **1. Skeleton.** `CPU_BACKEND_JIT` (`core/cpu/backends/jit/`), an
  interpreter passthrough sharing `Interp_State`. Select with
  `-DCPU_BACKEND=jit`; presets `native-jit`, `web-jit`; `voland-cli run
  --backend jit`; web backend id 3 (`CpuBackendId.Jit`).
  `ctest --preset native-jit`: 45/45.
- [ ] 2. Emitter, module loading, table install; ADD/SUB/MOV/B diff-tested under Node.
- [ ] 3. Integer ALU, branches, inline-softmmu loads/stores, NZCV.
- [ ] 4. Block chaining / regions. Measure.
- [ ] 5. FP/NEON; Silksong frame hashes match. Measure fps.

## Files touched outside the JIT's own

- `CMakeLists.txt`: `jit` added to the `CPU_BACKEND` option list.
- `core/CMakeLists.txt`: jit sources, `SWITCH_CPU_BACKEND_JIT`.
- `core/cpu/cpu.{h,c}`: registry entry.
- `core/stubs/wasm_entry.c`: `cpu_backend_id_ffi` returns 3 for jit.
- `platform/web/bindings/core.ts`: `CpuBackendId.Jit = 3`.
- `platform/cli/voland_cli.c`: `--backend jit`.
- `CMakePresets.json`: `native-jit`, `web-jit`.

## Measurements

None yet.

## Needs from the renderer side

Nothing yet.
