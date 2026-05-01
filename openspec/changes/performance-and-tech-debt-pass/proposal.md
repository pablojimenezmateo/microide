## Why

`WorkspaceShell` has grown into a 17 000-line god object spanning 36 translation units, 224 member variables, and 696 method declarations, creating a high-drag surface that slows every reviewer, risks regressions on every merge, and obscures real per-frame inefficiencies in the render and text paths. The cost of leaving it is compounding now that platform seams and plugin boundaries are solidifying.

## What Changes

- Break `WorkspaceShell`'s responsibilities into narrower, self-owned coordinator or service types so no single object coordinates rendering, input, terminal, compare, search, git, plugins, and session state simultaneously.
- Reduce `WorkspaceShell.h` include footprint (currently 71 `#include` directives) so that touching one subsystem no longer rebuilds the entire workspace layer.
- Eliminate per-call heap allocations in `TextRenderer::TruncateToWidth` (currently allocates a `std::vector<std::size_t>` on every truncation call) and bound the unbounded `mutable std::unordered_map` width cache.
- Deduplicate the 28 `Render*` and 20+ `RequestRedraw*` patterns scattered across `WorkspaceShell*.cpp` files that share near-identical structure.
- Clarify the ownership boundary between `AsyncSubprocess` and `AsyncProcessBackend`; the wrapper adds no value over a direct `CreateAsyncProcessBackend()` call at the use site.
- Update `performance-budgets` spec to record the measurable improvements and new budget lines that this pass is expected to hold.

## Capabilities

### New Capabilities

- None.

### Modified Capabilities

- `performance-budgets`: Add or tighten budget lines for render-path allocation rate, `TextRenderer` cache hit rate, and WorkspaceShell compilation unit size after the refactor.

## Impact

- Affected code: `src/workspace/WorkspaceShell*.{h,cpp}` (all 36+ files), `src/render/TextRenderer.{h,cpp}`, `src/platform/AsyncSubprocess.{h,cpp}`, `src/platform/ProcessBackend.{h,cpp}`.
- Tests: existing WorkspaceShell and TextRenderer test fixtures must remain green; redraw-comparison tests in `tests/WorkspaceShell*.cpp` provide the regression baseline.
- Build times: include-footprint reduction should measurably lower incremental rebuild cost for workspace-layer changes.
- No public API or plugin contract changes; this is an internal structural pass.
