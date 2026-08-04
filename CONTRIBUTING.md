# Contributing to microide

Thanks for looking. This is a small, opinionated project with strong conventions —
reading this first will save you a round trip.

## Before you start

microide is **100% vibecoded**: every source file, test, and document was written by
AI coding agents under human direction (see [README](README.md#about)). Contributions
from humans and agents are equally welcome; the bar is the same either way, and it is
the bar described below, not the origin of the diff.

Two documents own the rules:

- **[`AGENTS.md`](AGENTS.md)** — engineering policy, priority order, and the
  do-not-regress patterns. This is the top of the source-of-truth stack.
- **[`CLAUDE.md`](CLAUDE.md)** — the operating guide: where things live, how to
  build and test, and the hard architectural invariants that a lint enforces.

If those disagree with anything here, they win.

## The priority order

Changes are judged against this order, in this sequence:

1. **speed** — latency is the product
2. **correctness** — never traded away for the three below
3. low CPU usage
4. low memory usage
5. architectural clarity
6. compatibility, only when explicitly required

A fast path that is wrong on a routine input is not a valid contribution. A correct
path that regresses a measured hot path needs a measurement, not an argument.

## Build and test

```sh
cmake -S . -B build
cmake --build build --target microide_tests -j"$(nproc)"   # inner loop: tests only
ctest --test-dir build --output-on-failure -j"$(nproc)"
```

Prefer the logging wrapper — it tees everything to a file you can read back without
rerunning:

```sh
tools/run-checks.sh tests        # -> /tmp/microide-tests.log
tools/run-checks.sh perf-tests   # allocation-counting assertions armed
tools/run-checks.sh coverage     # line coverage + per-area floors
tools/run-checks.sh asan         # also: ubsan, tsan
tools/run-checks.sh fuzz         # build + smoke the 12 fuzz targets
tools/run-checks.sh clang-build  # whole tree, clang, warnings-as-errors
tools/run-checks.sh perf-canary  # proves the perf gate can still fail
```

`clang-build` is worth running before you push if you added a source file to a
curated target list (the bench and fuzz binaries name their sources explicitly) or
touched anything the default `tests` build does not compile — it is the only lane
that builds `src/app/main.cpp`, `microide_perf` and the benches with clang, and the
only one where a warning fails the build.

**SDL3 is not packaged by Debian or Ubuntu.** Build it once with
`scripts/ci/install-sdl3-linux.sh` (the same script CI uses). Everything else comes
from the archive; see [README § Build](README.md#build).

Linux is the only supported host. macOS and Windows are not build targets.

## What a good change looks like

- **Every bug fix adds or tightens a regression test.** "Should be covered already"
  is not accepted — if it were covered, the bug would not have shipped.
- **Tests assert an oracle, not just absence of a crash.** A test that would still
  pass if the code under it never ran is worse than no test, because it reports
  coverage that does not exist. See
  [`dev-docs/project/validation-traps.md`](dev-docs/project/validation-traps.md) —
  it is the most useful document in this repo and it is short.
- **New lint rules carry positive *and* negative control fixtures.** Three
  architecture rules were once structurally incapable of firing and passed green for
  months. Probe your rule by injecting a synthetic violation before trusting it.
- **Performance claims are measured.** `MICROIDE_PERF_TRACE=1` before and after, or a
  scenario in `tests/perf/`. Code review does not confirm performance impact, and
  neither does LTO.
- **Broad refactors are fine** when they improve correctness or subsystem ownership.
  Do not preserve a stale boundary out of politeness. Do not add a compatibility shim
  around one either — fix the boundary.

## What will get a change rejected

- Reintroducing anything in **CLAUDE.md § Hard Architectural Invariants**. Most are
  enforced by `tests/ArchitectureInvariantsTests.cpp` and will simply fail; the rest
  are reviewer-enforced.
- Widening `WorkspaceShell`. If your work wants shell access, the answer is a
  narrower registry, coordinator, or service — not another member.
- Rendering from plugins. Plugins contribute data, commands, providers, or structured
  requests; the host owns drawing, redraw policy, layout, and chrome.
- Marketing language in code, commits, or docs. The project has internal regression
  baselines and no third-party comparative benchmarks; claims stay inside that.

## Commits

- Coherent commits over large mixed snapshots. Each should be a defensible step.
- Say plainly what changed and **why**, including what you ruled out. The commit log
  is the primary record of intent here.
- Name a major refactor in the message instead of burying it.
- A changed perf baseline must carry a `perf-baseline:` line explaining it — CI
  enforces this.

## Filing a bug

Include the microide version (`microide --version`), your distro, and whether you
installed the `.deb` or built from source. If it is a rendering or input issue, say
whether you are on X11 or Wayland.

From the README: *"If you find a bug or a limitation that is not listed, that itself
is a bug — please file it."* That still stands.

## Where to look next

| you want | read |
| --- | --- |
| what to work on next | [`dev-docs/project/active-work.md`](dev-docs/project/active-work.md) |
| why a subsystem is shaped this way | [`dev-docs/project/implementation-guide.md`](dev-docs/project/implementation-guide.md) |
| durable behavioural contracts | [`openspec/specs/`](openspec/specs/) |
| open, actionable debt | [`dev-docs/project/known-tech-debt.md`](dev-docs/project/known-tech-debt.md) |
| when a green run stops being evidence | [`dev-docs/project/validation-traps.md`](dev-docs/project/validation-traps.md) |
| the handbook | [`guidelines/`](guidelines/) |

## License

By contributing you agree your work is licensed under the repository's
[LICENSE](LICENSE).
