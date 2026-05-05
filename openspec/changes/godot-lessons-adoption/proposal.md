## Why

Godot Engine is a mature C++ project with a long-standing reputation for tight footprint, fast iteration, and disciplined code health. After surveying its codebase against microide's stated priorities (correctness > speed > low CPU > low memory) and architectural stance (host-owned rendering, narrow plugin seams, no god-objects), six patterns are concretely applicable to microide today. Adopting them now — before the editor and plugin surface grow further — locks in correctness invariants and CI guards that would be costlier to retrofit later. The remaining Godot patterns (custom string types, `Object`/`Variant`/`Callable` reflection, scripting-scale allocators, GDExtension ABI, scene tree, single-compilation-unit builders) are deliberately rejected as out of scope for a small single-window IDE — they exist to solve game-engine-scale problems microide does not have.

## What Changes

- **Editor — gutter registry**: Replace the per-feature gutter wiring (blame, diagnostics, breakpoints, fold marks, etc.) with a single host-owned `GutterRegistry` indexed by gutter id, with typed columns (text / icon / custom-draw callback). Plugins and host subsystems contribute columns through the registry; the editor host owns layout, hit testing, and paint order. **BREAKING** for any plugin code that today reaches into editor render internals to draw gutters — those paths must migrate to the registry.
- **Editor — per-line shape/measurement cache with explicit dirty flag**: Each editor line owns its measured-text cache (width, glyph runs, wrap points). An edit flips the line's `measure_dirty` flag; rendering is forbidden from mutating measurement and only consumes cached data. Replaces ad-hoc measurement done during paint and removes the implicit "render path may re-measure" assumption.
- **Tooling — `MICROIDE_FAIL_*` precondition macros**: Add a small header (`util/Fail.h`) with non-throwing precondition macros (`MICROIDE_FAIL_INDEX_V`, `MICROIDE_FAIL_COND_V`, `MICROIDE_FAIL_NULL_V`, etc.) that assert in debug builds and log+return in release builds. Runtime-toggleable so test failure-path coverage stays quiet. Aligns with the existing no-exception policy enforced in `util/Parse.h`.
- **Tooling — scoped `LogSilencer` RAII guard + `EventRecorder` test helper**: A scoped silencer for negative-path tests that intentionally trigger logs, plus a recorder that captures the dispatched events / state notifications produced by host services between two checkpoints so tests can assert on the full sequence rather than just final state.
- **Style/CI — three Python lint guards**: Add `scripts/lint/check_header_pragma.py`, `scripts/lint/check_file_format.py`, `scripts/lint/check_include_form.py` (header `#pragma once`, BOM/EOL/trailing whitespace/final newline, include-path normalization). Wire them into CTest and a CI job. A small `.clang-tidy` is added with a deliberately tiny check set (≤10 high-signal checks, e.g. `bugprone-use-after-move`, `readability-braces-around-statements`, `performance-move-const-arg`) gated as a non-blocking CI job.
- **CI — scope static checks to changed files**: Use `tj-actions/changed-files` (or the equivalent) to scope architectural-lint, clang-format, and the new Python lints to only the files changed in the PR.

Explicitly **not** included (rejected from Godot survey):
- Custom string types (`String`/`StringName`/`CowData`).
- `Object`/`Variant`/`Callable`/signals reflection layer.
- Scene-tree, `Resource`/`Ref<T>`, "servers" architecture, RID model.
- GDExtension-style plugin ABI (microide's Lua plugin seam stays).
- SCU (single-compilation-unit) builders.
- Custom paged allocators / `LocalVector` wholesale replacement of `std::vector` (left on watch-list pending profiler evidence).
- Heavy i18n/translation server.

## Capabilities

### New Capabilities
- `editor-gutter-registry`: Host-owned registry of editor gutter columns (text / icon / custom-draw), plus the per-line shape/measurement cache with an explicit dirty flag that the registry's renderers consume.
- `style-and-lint-guards`: Repository-wide style and include-form lint checks (Python-driven), plus a curated `.clang-tidy` profile, plus the CI wiring that scopes these checks to changed files.

### Modified Capabilities
- `bug-detection-tooling`: Add requirements for the `MICROIDE_FAIL_*` precondition macro family, the scoped `LogSilencer` RAII guard, and the `EventRecorder` test helper. No existing requirements are weakened.

## Impact

- **New code**: `src/util/Fail.h`, `src/editor/GutterRegistry.{h,cpp}` (or chosen module location), per-line measurement cache fields on the existing line type, `tests/util/LogSilencer.h`, `tests/util/EventRecorder.h`, `scripts/lint/*.py`, `.clang-tidy`.
- **Refactored code**: Existing gutter contributors (blame, diagnostics, breakpoints, fold marks) migrate to `GutterRegistry`. `TextRenderer` and the editor render path stop performing measurement during paint and read only from the per-line cache. Hot redraw / scroll paths benefit from removed re-measurement work.
- **CI**: New static-checks job (changed-files-scoped) running file-format / header-pragma / include-form / clang-format / clang-tidy. Existing CTest run gains the Python lint targets as additional cases.
- **Plugin contract**: The Lua plugin host gains a `gutter.add(...)` style API; any pre-existing path that drew custom gutters by reaching into render internals is removed (covered by the **BREAKING** flag above).
- **Architecture invariants**: A new lint-test (or extension of `tests/ArchitectureInvariantsTests.cpp`) enforces that editor render TUs do not call measurement APIs and only read cached widths from lines.
- **Docs**: `docs/active-work.md` notes the adoption phase; `guidelines/cpp.md` gains the precondition-macro guidance; `guidelines/testing.md` documents `LogSilencer`/`EventRecorder`; `guidelines/ui-shell.md` documents the gutter registry as the only seam for adding columns.
- **No runtime dependency changes** (no new third-party libs).
