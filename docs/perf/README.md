# Perf tracking

`silksong.csv` has one row per measured commit and page configuration,
written by `platform/web/tools/perf-track.mjs`. Run it after a change that
could affect speed.

Each row covers two runs in Chromium on the host GPU (`tools/perf.mjs`):

- **Title:** warm up to slice 860k, then measure for 30 s.
- **Gameplay:** press A (`z`) at slices 860k, 940k, 1.02M, 3.0M, 3.1M and
  3.2M (title, profile, New Game, video, prompt), with load phases
  recorded, to slice 4.8M, then measure for 30 s.

Machine load varies between runs. Watch the
`*_cpu_per_vs` columns: renderer CPU seconds per guest (virtual) second.
They are much less sensitive to load than the wall-time columns. A merge
that raises any of them by more than 10% is reported as a regression.

Rows compare only with rows of the same `preset`. `web` is the shipping
default page; `web?cores=0` is the same build forced onto the serial
scheduler.
