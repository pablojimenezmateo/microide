# Perf Harness

The perf harness is the primary regression oracle for startup and interactive-performance changes.
It runs scenario workloads through `microide_perf` and compares measured aggregates against committed
baselines.

## Configure

```bash
cmake --preset microide-perf
cmake --build build/microide-perf-make -j8
```

`microide-perf` enables:

- `CMAKE_BUILD_TYPE=RelWithDebInfo`
- `MICROIDE_WARNINGS_AS_ERRORS=ON`
- `MICROIDE_PERF_HARNESS_BUILD=ON`

## Scenario Authoring

Scenarios are registered in `tests/perf/PerfMain.cpp` and use `ScenarioContext` helpers from
`tests/perf/PerfHarness.{h,cpp}`.

When adding a scenario:

1. register a unique scenario name
2. keep setup deterministic (fixtures + explicit waits)
3. drive behavior through context helpers (`Open`, `OpenTab`, `Type`, `Scroll`, `KeyDown`, `Wait`)
4. pump frames intentionally (`PumpFrames`) so rendering work is included consistently
5. decide whether the scenario belongs in smoke (`.smoke = true`) or gate-only (`.smoke = false`)

## Run Under Virtual Display

```bash
xvfb-run -a ./build/microide-perf-make/microide/microide_perf --smoke
```

If SDL fails to initialize under Xvfb, force the expected environment variable names:

```bash
xvfb-run -a env SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy \
  ./build/microide-perf-make/microide/microide_perf --smoke
```

Use `SDL_VIDEODRIVER` (not `SDL_VIDEO_DRIVER`).

## Baseline Workflow

Per-scenario baselines are stored under `tests/perf/baselines/<scenario>.json`.

Update one or more baselines:

```bash
xvfb-run -a env SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy \
  ./build/microide-perf-make/microide/microide_perf \
    --scenarios=<comma-separated-scenarios> \
    --iterations=10 \
    --update-baseline
```

Check against existing baselines:

```bash
xvfb-run -a env SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy \
  ./build/microide-perf-make/microide/microide_perf --iterations=10
```

## Baseline Change Rule

If a change modifies any `tests/perf/baselines/*.json`, the change record must include a line that
starts with:

```text
perf-baseline: <reason>
```

Accepted locations:

- PR description
- Commit message

The `perf-baseline-tag` CI job enforces this.

## Reference Runner Class

The gate run is measured on a dedicated self-hosted runner class tagged `perf-runner-v1`. Baselines
must be updated from this class (or a machine with equivalent characteristics) before tightening or
replacing gate numbers.

## Smoke vs Gate Split

- local/PR smoke: small subset for quick signal (`microide_perf_tests` / `--smoke`)
- perf gate: full suite on `perf-runner-v1` with baseline comparison
