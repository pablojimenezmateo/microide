# MicroIDE Known Tech Debt

The queue of debt that is open, actionable and still present in the tree. Closed
debt does not live here: when an entry is resolved, decided or won't-do, it moves
to the dated ledger under `guidelines/tech-debt/archive/` (see "Where the history
lives" at the end), so the open work is not buried among the record of the closed.

Reviewed 2026-11-03: the file had drifted back to a record -- 290 of its 294 sections
were resolved, closed or decided, and the head of the file was ~500 lines of pass
summaries. They moved, verbatim, to
`guidelines/tech-debt/archive/2026-11-03-resolved-debt-ledger.md`; the four open
entries and the guardrails stayed.

Reviewed 2026-11-08: the eight entries resolved or closed since (TD-406, 408, 429-434)
moved, verbatim, to `guidelines/tech-debt/archive/2026-11-08-resolved-debt-ledger.md`.

## Open items

None. (2026-11-08: the last open entries were resolved and moved to the ledger.)

## Guardrails — rejected experiments, do not retry

These are dead ends proven by the perf gate. Re-attempting them in the same shape wastes effort and
the gate will reject them again.

- **Preferring the Vulkan render driver for a faster launch** (2026-08-17). `SDL_CreateRenderer`
  costs 63-80 ms with SDL's own pick here (`opengles2`) and 41 ms with `vulkan`, and
  `SDL_HINT_RENDER_DRIVER` takes a comma-separated fallback list, so the ~30 ms looked free. It is
  paid back immediately: `render::TextBackend::UploadStringTexture` measures **8.9-10.6 ms per 95
  uploads on opengles2 and 27.1-29.0 ms on vulkan** — 3x, on the shell thread, on a path that runs
  on every glyph-cache miss. Trading interactive latency for a one-time 30 ms is the wrong direction
  for a project whose first priority is latency. Do not retry without re-measuring the upload path;
  the startup number is not the one that decides it. (`software` also takes ~75 ms to create here,
  which is the tell that the cost is the Mesa/EGL load rather than anything GL-specific — there is
  no cheap driver to switch to, only a different trade.)

- **Pre-warming the GL stack on a background thread** (2026-08-17). `SDL_CreateRenderer` is
  60-90 ms and traces almost entirely to Mesa's `eglInitialize` — a dlopen chain of 35 shared
  objects including libLLVM, plus DRM ioctls. The shell thread is idle before it, so warming the
  stack from a thread started at the top of `Application::Initialize` looked like most of that
  moved off the critical path. Measured, ten interleaved rounds each, time from process start to a
  created renderer: **108.8 ms cold vs 121.2 ms warmed** — worse. The warm does cut SDL's own
  renderer creation (86 -> 58-64 ms), but it costs 62-66 ms to do it and only ~20 ms of that
  overlaps anything (`SDL_Init` plus window creation), so the join eats the rest. Warming with
  `dlopen` alone and no `eglInitialize` costs 0.4 ms and saves nothing (99.5 vs 102.8 ms): the
  expensive part is the driver initialization, not loading libEGL. And an EGL display we initialize
  on the default (X11/DRM) native display is not the one SDL initializes on the Wayland one, so
  part of the work is done twice by construction. Do not retry without a way to warm *SDL's own*
  display before it needs it.

- **Deferring VSync until the window is mapped** (2026-08-17). The renderer is created with VSync on
  and the first two presents (the startup frame, and the re-blit after `SDL_ShowWindow`) happen
  while the window is still hidden, so pacing them to a refresh boundary looked like up to a frame
  interval each of pure waste. Measured five interleaved launches each on a Wayland session: time to
  `Application::FirstRender` **104.9 ms vs 105.9 ms** — no effect. A present against an unmapped
  surface does not block for a frame callback that was never requested, so there was nothing to
  save. Reverted rather than kept "because it cannot hurt": it is a second VSync state to reason
  about for zero measured gain.

- **Capacity-preserving line assembly in `PieceTree::ExtractLineRange`** (2026-08-10). The spanning
  line path does `out.push_back(std::move(current))`, which hands the buffer away and leaves `current`
  at zero capacity, so the next line that spans pieces re-allocates and grows geometrically. Copying
  out of `current` instead (and constructing single-piece lines straight into `out`) makes it exactly
  one allocation per line in every case. Measured **zero** on `editor_long_line_select_all_edit`,
  `editor_typing_minified_line`, `editor_smart_indent_typing`, `editor_sort_lines_large`,
  `first_line_edit`, `mid_file_edit`, `typing_large_file`, `toggle_line_comment` — before and after,
  same build, identical counts. The reason is structural: an insert appends its whole text as ONE
  piece, so a multi-line slice's lines do not span pieces; only an intra-line edit splits a line, and
  the paths that slice after those edits record inline undo entries instead. The shape the fix
  addresses is not produced by the editor. Do not retry without first exhibiting a scenario whose
  slice actually spans pieces.

- **Editor glyph atlas on the draw path** (GPU / `SDL_RenderGeometry`). RESOLVED 2026-06-28 on
  `perf/gpu-render-path`: the three preconditions were met with measurement (GPU backend confirmed +
  measurable via the new `--renderer=auto` advisory lane; the `editor_scroll_fresh_content_large`
  sweep shows ~9.7% texture-cache miss with heavy eviction churn), and a **GPU-gated, row/gutter-batched**
  atlas shipped — pixel-identical to the composite path (0-pixel-diff certified on `opengles2`),
  −8% to −15% on heavy text scenarios, software path unchanged, default-on for GPU with
  `MICROIDE_RENDER_GLYPH_ATLAS=0` as escape hatch. The original *per-quad* shape stayed wrong (it
  regressed whitespace +27% by flapping the batcher); the fix was batching per row + per gutter flush.
  The 2026-05-15 +48–83% figures were a software-renderer artifact (`SDL_RenderGeometry` rasterizes
  per-pixel there). Detail:
  `guidelines/tech-debt/archive/2026-06-16-terminal-headless-and-glyph-atlas-closeout.md` (§13 Update
  2026-06-28). (The endorsed *miss-path* colour-independent coverage atlas also remains —
  `src/render/AsciiGlyphAtlas.{h,cpp}` — now also the GPU atlas's texture source.)
- **`TextDocumentModel` ownership extraction** from `TextViewport`. Rejected: it regressed hot
  editor/render scenarios (~+15% to +30% wall) and broadly increased allocations. Do not reintroduce
  in the same shape without first proving line access, mutation, revision updates, and cache
  invalidation are allocation-free and performance-neutral in the editor benchmarks. Detail:
  `guidelines/tech-debt/archive/2026-05-20-textviewport-and-shell-decomposition.md`.
- **`UNITY_BUILD` on `microide_tests`** (build-speed). Rejected 2026-06-29: 116 of 121 test TUs
  define helpers in anonymous namespaces, with confirmed same-name collisions across files
  (`MakeService`, `MakeViewport`, `MakeFixtureRoot`, …). CMake's unity batching concatenates files
  into one TU without isolating their unnamed namespaces, so those become colliding
  `(anonymous namespace)::` symbols → redefinition errors. Making it compile would mean rewriting
  ~116 files (per-file `UNITY_BUILD_UNIQUE_ID` namespace wrapping) for a speculative win the PCH pass
  below already captured the safe part of. Do not retry without first solving the anonymous-namespace
  collision mechanically. The shipped win instead was **precompiled headers** (stable std + SDL set,
  shared by `microide`/`microide_tests`/`microide_perf`): clean `microide_tests` build 141.3 s → 120.0 s
  (~15%), suite still green, runtime byte-for-byte unchanged.

## Shipped build-speed wins (2026-06-29, after the PCH pass)

- **Shared `microide_core` object library.** `MICROIDE_CORE_SOURCES` (≈373 TUs) is now an
  `OBJECT` library compiled once and spliced (`$<TARGET_OBJECTS:microide_core>`) into
  `microide`/`microide_tests`/`microide_perf`, instead of being re-listed in each target. This
  removed the previous double compile of the entire core. The prerequisite was making core
  **`MICROIDE_TESTING`-free**: ABI-neutral seams (`*ForTesting` methods, `TestAccess`/test friends,
  the `before_cache_apply_hook`, `test_sent_bytes_`) are now compiled unconditionally, and the
  genuinely behavioral forks (placeholder-vs-real terminal startup, project-init default terminal,
  `SendBytes` capture) are gated at **runtime** via `terminal::SetUsePlaceholderTerminalsForTesting`
  (the test/perf `main()` enables it), mirroring the existing `SetHostPlatformOverrideForTesting`
  precedent. Core must stay free of `#ifdef MICROIDE_TESTING`, or the shared object set diverges by
  ABI between binaries. Measured: clean build of `microide` + `microide_tests` 148 s → 91 s (~38%,
  ccache disabled for the comparison). `WorkspaceShellTestAccess.h` keeps its guard — it is a
  test-only header, never compiled into core.
- **ccache + ld.lld + split-dwarf**, all conditionally enabled in `CMakeLists.txt` (no-ops when the
  tool is absent; toggle with `-DMICROIDE_USE_CCACHE=OFF` / `-DMICROIDE_USE_LLD=OFF`). lld is skipped
  under LTO so the `microide-perf` preset keeps its default linker. `tools/run-checks.sh` exports
  `CCACHE_SLOPPINESS=pch_defines,time_macros` so PCH TUs cache.
- **Scoped inner-loop build.** `tools/run-checks.sh tests` and the documented loop build
  `--target microide_tests` (the only binary `ctest` invokes), skipping `microide` and the bench
  binaries.
- **Splitting `microide_tests` into multiple filtered `add_test` invocations for `ctest -j`**
  (test-run speed). Rejected 2026-06-29: partitioning ~121 files' worth of tests by name-substring
  filters is a silent-coverage hazard — a test matching no subset is dropped from the run with no
  signal, trading a correctness guarantee for ~2× wall-clock on an already-acceptable 47 s suite. The
  in-binary substring filter (`TestRunnerCli`) already covers focused local iteration. Do not retry
  without a mechanically-proven complete-and-disjoint partition (e.g. generated from the registry, not
  hand-maintained filter strings).

## Where the history lives

The detailed record of closed debt — the 2026-04-29 comprehensive cleanup, the render/layout perf
batch, the throughput-pass follow-ups, the layout-revision tiers, the event-driven search/index
work, the `TextViewport` / `WorkspaceShell` decomposition, the 2026-06-11 deep correctness audit, the
2026-06-15/16 render/app/util/terminal closeouts, and the 2026-07-12 deferred-backlog sweep (the
cross-subsystem bug-hunt passes 5–24 closeout) — now lives in:

- `guidelines/tech-debt/archive/` — per-pass archive records (with reproduction notes and lessons),
  and the ledgers of entries resolved here: `2026-08-04-resolved-debt-ledger.md`,
  `2026-11-03-resolved-debt-ledger.md` (every TD id from 2026-08-04 to 2026-11-02 not listed
  above, with the per-pass review summaries that headed this file) and
  `2026-11-08-resolved-debt-ledger.md` (TD-406, 408, 429-434)
- `CHANGELOG.md` — shipped, user-facing release history
- `openspec/changes/archive/` — the full proposal/spec/tasks record per shipped change

The broader 2026-04-20 architectural review is archived at
`dev-docs/archive/production-tech-debt-review.md`.
