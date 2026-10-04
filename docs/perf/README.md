# Perf tracking

`silksong.csv` has one row per measured `local/dev` commit and page
configuration. `platform/web/tools/perf-track.mjs` writes it; bot 4 runs it
for each merge.

Each row covers two runs in Chromium on the host GPU (`tools/perf.mjs`):

- **Title:** warm up to slice 860k, then measure for 30 s.
- **Gameplay:** the BOTS.md button recipe, with load phases recorded, to
  slice 4.8M, then measure for 30 s.

The machine is shared and its load swings from 20 to 180. Watch the
`*_cpu_per_vs` columns: renderer CPU seconds per guest (virtual) second.
They are much less sensitive to load than the wall-time columns. A merge
that raises any of them by more than 10% is reported as a regression.

Rows compare only with rows of the same `preset`. `web` is the shipping
default page; `web?cores=0` is the same build forced onto the serial
scheduler.
