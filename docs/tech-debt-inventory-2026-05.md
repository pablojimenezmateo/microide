# Technical debt inventory (May 2026)

Snapshots evidence gathered with repository search tools (`grep`, `wc -l`, targeted reads) on the tree at `/home/gef/Documents/projects/microide`. Priorities reflect risk to correctness, architecture compliance, and maintainability—not product roadmap.

## Executive summary

- **Repository hygiene:** Over 600 files under `build-perf/` are tracked by git (CMake build outputs and dependency files). They should not live in version control; they stale-reference deleted sources (for example `WorkspaceSecretStorage.cpp`) and inflate diffs and clones.
- **Policy versus lint coverage:** `WorkspaceShellMembers.inc` declares `friend class WorkspaceShell` and (under `MICROIDE_TESTING`) `friend struct TestAccess`, but `CheckWorkspaceFriends` in `tests/ArchitectureInvariantsTests.cpp` only scans `*.h` and `*.cpp` under `src/workspace/`, not `*.inc`. This is a **lint blind spot** relative to `AGENTS.md` (“no `friend` in `src/workspace/*`”).
- **Lua VM surface area:** `src/editor/SyntaxDefinitionLoader.cpp` embeds a full Lua stack (`luaL_newstate`, `lua_State*`, table walks) for runtime syntax when `MICROIDE_HAS_LUA_PLUGINS` is on. Narrow lint rules forbid `lua_State*` only in a fixed list of editor TUs; the stated global invariant in `AGENTS.md` is “no `lua_State*` outside `plugin/LuaRuntime`,” so the editor loader is an **intentional or legacy exception** that should be documented or reconciled.
- **Duplicate Lua helpers:** `LuaErrorString(lua_State*)` and `ReadStringField`-style table parsing appear in both `SyntaxDefinitionLoader.cpp` and `src/plugin/PluginLifecycleLoadInterop.cpp` / `PluginRegistrationParsers.cpp`—maintenance and drift risk.
- **Main-thread subprocess work:** `WorkspaceToolDownloader.cpp` calls `project::RunSubprocess` synchronously inside `ComputeSha256Blocking`; formatter execution in `WorkspaceTabCoordinatorShellBridge.cpp` posts to `ProjectBackgroundExecutor` but then **blocks the caller** with `formatter_future.get()`. Architecture rules emphasize avoiding shell-thread stalls; only `platform::RunSubprocess` is hard-linted.
- **Large translation units:** `src/editor/TextViewport.cpp` (~3.1k lines), `src/compare/CompareModel.cpp` (~1.2k lines), `src/platform/FileIndexWatcher.cpp` (~1.3k lines), `src/platform/ProcessBackend.cpp` (~1.1k lines), and several workspace TU’s in the ~550–990 line band are concentration points for regressions and review load.
- **Rendering policy vs. implementation:** `WorkspaceShellRenderChrome.cpp` allocates `std::string` via `std::to_string(hidden_count)` when drawing tab-overflow badges. The architectural-lint rule `CheckRenderTuDoesNotMaterializeStrings` only guards specific search/replace fallback patterns in `WorkspaceShellRenderSidebar.cpp`, so **hot-path string allocation** can still slip in other render TU’s.
- **Documentation drift:** `docs/active-work.md` still describes `WorkspaceSecretStorage.*`, `WorkspaceAuthProvider.*`, and Phase 4 AI-adjacent surfaces as present while corresponding sources are removed or retired in the current working tree; readers get a split-brain picture alongside `Phase 5 AI … retired` language.
- **Placeholder tests:** Multiple tests in `tests/WorkspaceShellChromeTests.cpp` and `tests/WorkspaceShellPluginTests.cpp` are reduced to `Expect(true, "… retired")`, shrinking real coverage after capability removals.
- **Low explicit marker debt:** A repo-wide search for `TODO` / `FIXME` / `HACK` / `XXX` in hand-written `src` (excluding `RuntimeSyntaxGenerated.cpp`) returned **no matches**—good discipline, but it means debt is structural rather than labeled.

## Table of contents

1. [Workspace / shell orchestration](#1-workspace--shell-orchestration)
2. [Editor and shared edit primitives](#2-editor-and-shared-edit-primitives)
3. [Diff / compare / merge](#3-diff--compare--merge)
4. [Plugins / Lua](#4-plugins--lua)
5. [Project services and persistence](#5-project-services-and-persistence)
6. [Platform, terminal, integration](#6-platform-terminal-integration)
7. [Rendering and UI text](#7-rendering-and-ui-text)
8. [Tests, tooling, and build](#8-tests-tooling-and-build)
9. [Quick wins](#9-quick-wins)
10. [Out of scope / not investigated](#10-out-of-scope--not-investigated)

---

## 1. Workspace / shell orchestration

### Critical

- **Tracked build tree `build-perf/`:** `git ls-files build-perf | wc -l` reports 616 tracked paths. These CMake artifacts reference obsolete compilation units and conflict with normal “build outputs are local” practice. Remove from the index, add an ignore rule, and rely on CI/local builds.

### High

- **Architectural `friend` in an `.inc` file:** `WorkspaceShellMembers.inc` grants friendship to `WorkspaceShell` for `FrameToken` and to `TestAccess` when testing is enabled:

```71:73:src/workspace/WorkspaceShellMembers.inc
   private:
    friend class WorkspaceShell;
    explicit FrameToken(std::uint64_t frame_id, VisibleLineRange visible_line_range)
```

  The architecture test only iterates `extension() == ".h" || ".cpp"`, so this does not fail the lint today. **Potential regression risk** if `.inc` inclusion is considered “workspace code” under policy.

- **Blocking work on save path:** `PrepareEditorViewportForSave` offloads the formatter subprocess to `project_background_executor_` but synchronously waits:

```137:150:src/workspace/WorkspaceTabCoordinatorShellBridge.cpp
    project_background_executor_.Post(
        [formatter_promise, formatter_cwd, formatter_command, formatter_input]() mutable {
      platform::SubprocessResult subprocess_result =
          project::RunSubprocess(formatter_command,
                                 platform::SubprocessOptions{
                                     .cwd = formatter_cwd,
                                     .stdin_text = formatter_input,
                                     .environment_overrides = {},
                                 });
      formatter_promise->set_value(std::move(subprocess_result));
    });
    platform::SubprocessResult result = formatter_future.get();
```

  This still blocks the calling thread until formatting completes (likely the SDL/main thread). **Potential regression risk** under the latency and “no shell-thread stalls” goals in `AGENTS.md`.

### Medium

- **Synchronous SHA helper:** `ComputeSha256Blocking` in `WorkspaceToolDownloader.cpp` invokes `project::RunSubprocess` for `sha256sum` / `shasum` directly—acceptable for a background utility if never called from the main loop, but worth auditing call sites for accidental UI-thread use.

- **Shell size budget headroom:** `WorkspaceShell.cpp` is ~442 lines (policy cap 600); `WorkspaceShell.h` ~109 lines (cap 400). Healthy today, but several adjacent TU’s (`WorkspaceShellRedraw.cpp` ~987, `WorkspaceActionServices.cpp` ~918) absorb orchestration growth **outside** the nominal shell files.

- **`PrepareFrameOnce` builds two `RenderViewModelBuilder` instances** for sidebar and bottom panel each frame (`WorkspaceShellRenderFrame.cpp` lines 67–69). Duplicated builder construction may be a small per-frame cost; worth profiling if idle CPU is sensitive.

### Low

- **No `// TODO`-style markers** surfaced in non-generated workspace sources—tracking work happens elsewhere (docs, issues) or not at all.

---

## 2. Editor and shared edit primitives

### High

- **`TextViewport.cpp` scale:** ~3k lines in a single TU. Even with lint guarding `ApplyLineEdit` / `ApplyRangeEdit` / `ReplaceAll` against full `document_->lines` snapshots, the file remains a **change-is-risky** hotspot.

- **`SyntaxDefinitionLoader.cpp` and Lua:** When plugins are enabled, the editor syntax loader owns Lua state for parsing embedded definitions (`luaL_newstate`, extensive `lua_State*` use). This diverges from the high-level “Lua lives behind `LuaRuntime`” story unless the policy is explicitly narrowed to “plugin execution VM only.”

### Medium

- **`RuntimeSyntaxRegistry.cpp` (~1.1k lines)** plus generated `RuntimeSyntaxGenerated.cpp` (~3.6k generated lines) split “hand” and “generated” logic; any hand edits near generation boundaries need discipline to avoid merge pain.

- **`EditorViewRenderer.cpp` (~930 lines):** Large renderer for the main editor surface; overlaps thematically with workspace compare/merge render paths—watch for duplicated layout math.

### Low

- **Marker scan:** No raw `TODO`/`FIXME` strings in `src/editor/` outside generated syntax tables.

---

## 3. Diff / compare / merge

### High

- **`CompareModel.cpp` (~1,165 lines):** Central model for compare rows; size alone is a review and testing burden. Bugs here propagate to merge and virtual-document flows.

### Medium

- **Workspace-side compare/merge render:** `WorkspaceShellCompare.cpp`, `WorkspaceShellCompareRender.cpp`, `WorkspaceShellMergeRender.cpp`, `WorkspaceShellMergeState.cpp` each sit in the ~530–680 line band—cohesive but distributed; refactors should preserve the “compare surface gated by view models” rule from `CLAUDE.md`.

### Low

- **No additional structural flags** surfaced in this pass beyond file scale and cross-module coupling noted above.

---

## 4. Plugins / Lua

### Critical / High (policy alignment)

- **Widespread `lua_State*` in `src/plugin/*.cpp`:** Expected for interop, but combined with `SyntaxDefinitionLoader.cpp` the **total Lua coupling surface** is larger than the “opaque handle” wording suggests. Worth a short architectural note clarifying which TUs may touch `lua_State*` and which must not.

- **Two registration parser TU’s plus query interop:** `PluginRegistrationParsers.cpp` (~719 lines), `PluginProviderRegistrationParsers.cpp` (~335 lines), and `PluginProviderQueryInterop.cpp` (~781 lines). Similar `ReadStringField` helpers are repeated across files (see grep hits for `ReadStringField(lua_State*`).

### Medium

- **`PluginHost.cpp` footprint:** ~258 lines in the grep listing; header `PluginHost.h` ~428 lines. Within the 800-line-per-plugin-TU lint cap but trending toward another coordination hub.

### Low

- **Lint coverage:** Plugin TU size and `lua_State` bans in specific editor modules are enforced; broader “no raw Lua in editor” is not.

---

## 5. Project services and persistence

### Medium

- **`GitBlameService.cpp` (~754 lines)** is the largest service-style TU under `src/project/`; blame parsing and caching complexity tends to accumulate here.

- **Persistence layering is comparatively tight:** `PersistenceService.{h,cpp}` (~135 lines implementation) fronts `src/persistence/PersistedRecord*.{h,cpp}` with `PersistedRecord.cpp` at ~360 lines—reasonable separation.

### Low

- **No stray `std::sto*` usage** was detected in a quick `src` scan for `std::stoi`/`stoll`/`stoull`/`stof`/`stod` in this audit pass (empty result)—consistent with the Parse-helper policy.

---

## 6. Platform, terminal, integration

### High

- **`FileIndexWatcher.cpp` (~1,268 lines)** and **`ProcessBackend.cpp` (~1,105 lines)** are large platform bridges; they aggregate OS-specific backends and error paths—natural bulk, but risky for subtle threading bugs.

- **`AsyncSubprocess.cpp` (~742 lines)** and **`Subprocess.cpp` (~576 lines)** together define process semantics relied on by workspace and project layers.

### Medium

- **`TerminalBackend.cpp` (~642 lines)** backs PTY behavior; terminal correctness is correctness-first per project priorities—regressions are expensive.

### Low

- **No `SDL_PollEvent(0)`** zero-spin pattern was found in `src/workspace/` in this pass.

---

## 7. Rendering and UI text

### Medium

- **Chrome badge string allocation:**

```46:52:src/workspace/WorkspaceShellRenderChrome.cpp
  if (hidden_count > 0) {
    const std::string count_text = std::to_string(hidden_count);
    const float count_x = rect.x + rect.w * 0.55f;
    const SDL_FRect count_rect{count_x, rect.y,
                               std::max(0.0f, rect.x + rect.w - count_x - 2.0f), rect.h};
    DrawVCenteredTextOn(text_renderer, renderer, count_rect, 0.0f, foreground, background,
```

  **Potential regression risk** versus the “render hot paths avoid ephemeral `std::string`” guidance; the current lint does not flag this pattern.

- **`Theme.cpp` (~776 lines)** and **`SdlTtfTextBackend.cpp` (~497 lines)** hold most SDL_ttf-facing complexity; keep text measurement and caching deterministic per performance docs.

### Low

- **`RenderViewModelBuilder.cpp` (~708 lines):** Primary aggregation point for host-owned view models; growth here is preferable to logic in individual `WorkspaceShellRender*` TU’s.

---

## 8. Tests, tooling, and build

### Critical

- **Committed `build-perf/` tree** (see workspace section)—a tooling/process defect, not application logic.

### High

- **Placeholder “retired” tests** neutralize former AI/chat coverage—for example:

```521:527:tests/WorkspaceShellChromeTests.cpp
void TestWorkspaceShellChatComposerKeysDoNotLeakIntoEditor() {
  Expect(true, "chat composer is retired");
}

void TestWorkspaceShellChatComposerSupportsMultilineDraftsPerConversation() {
  Expect(true, "chat conversation drafts are retired");
}
```

  Similar patterns appear in `tests/WorkspaceShellPluginTests.cpp` for provider plugins. **Debt:** stale test names and registry entries that no longer assert behavior.

### Medium

- **Docs versus tree:** `docs/active-work.md` Phase 4 bullets still mention `WorkspaceSecretStorage.*` and `WorkspaceAuthProvider.*` as shipped components while other sections state Phase 5 AI surfaces are retired—requires editorial reconciliation after source removals land.

- **`openspec/` archive and specs** reference historical `WorkspaceProviderBridge` / `WorkspaceAiContext` contracts; fine as history, but easy to confuse with “current law” when `openspec/specs/` is not updated in lockstep.

### Low

- **Fuzz / perf harness:** This audit did not re-run `ctest` or fuzz targets; references remain in `AGENTS.md` / `docs/perf-harness.md` as operational expectations.

---

## 9. Quick wins

- Remove `build-perf/` from git tracking; add `build-perf/` (or unify under ignored `/build/`) in `.gitignore`; document the perf build directory in `docs/perf-harness.md` if a dedicated path is still desired.
- Extend `CheckWorkspaceFriends` to include `*.inc` under `src/workspace/`, or eliminate `friend` by narrowing `FrameToken` construction privileges another way.
- Replace placeholder `Expect(true, "retired")` tests with either deletion from the registry or concise `ArchitectureInvariants`-style checks that assert the absence of removed commands/UI hooks—restore signal, not noise.
- Refresh `docs/active-work.md` Phase 4/5 sections to match the filesystem (remove or mark removed modules explicitly).
- Move tab-overflow numeric badge text to a small fixed buffer or precomputed digit draw path, or prepare the string in `RenderViewModelBuilder` if it becomes per-frame hot.
- Consolidate Lua table `ReadStringField` / `LuaErrorString` helpers shared between `SyntaxDefinitionLoader` and plugin interop into one internal helper TU to prevent semantic drift.

---

## 10. Out of scope / not investigated

- Runtime performance validation (perf baselines, Tracy captures) and memory sanitizer matrices were not executed in this pass.
- Security review of plugin sandboxes, network surfaces, and tool download trust chain beyond noting synchronous subprocess use.
- exhaustive third-party dependency audit.
- Line-by-line review of every `tests/*.cpp` file beyond grep-driven samples.
- User-facing documentation outside `docs/active-work.md` snippets cited here.

---

*Inventory prepared as a point-in-time audit; line counts come from `wc -l` on named files and may shift as the branch evolves.*
