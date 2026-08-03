# Merge-Candidate Bug Audit - 2026-07-08

Branch: `cleanup/dedup-shared-primitives`
Commit audited: `611786be`

This audit treats the current branch as the merge candidate for `main`. Findings are ordered by
the requested priority: correctness bugs first, then speed pitfalls, then deduplication and
technical debt. "Confirmed" means the issue is visible from code inspection or an existing test
contract; it does not mean a failing regression test already exists.

Validation run during the audit:

- `tools/run-checks.sh tests` passed: `microide_tests`, `microide_perf_fixtures`, and
  `microide_perf_tests` were all green. Log: `/tmp/microide-tests.log`.

Agent coverage note:

- Two initial explorer agents completed and four initial explorer agents failed under the workspace
  spend cap. The missing slices were rechecked locally, and replacement focused explorers were
  started for editor/render, search/git/project/platform/terminal, and plugin/runtime code.

## Workspace And Persistence

### High - Cleared debug state can resurrect from `debug.bak`

Status: confirmed by code inspection.

When there is no debug state to persist, `PersistenceCoordinator::SaveDebugState` removes only the
primary `debug` file and returns (`src/workspace/persistence/WorkspacePersistenceCoordinatorSession.cpp:79`).
`PersistedRecordReader::ReadFile` falls back to `PersistedRecordWriter::BackupPathFor(path)` when
the primary file is missing (`src/persistence/PersistedRecordReader.cpp:149`). If a previous save
created `debug.bak`, clearing all debug state can make the next restore load stale breakpoints,
launch configs, watch expressions, exception filters, or function breakpoints from the backup.

Recommended fix: clear or tombstone through `PersistenceService` so primary and backup state agree.
Add a regression that saves non-empty debug state twice, clears it, restores, and asserts no backup
state returns.

### Medium - Compare review/staging state is written to memory but not to the binary record

Status: confirmed by code inspection.

`BuildPersistedCompareTabState` fills `compare_review_mode` and `compare_staging_view`
(`src/workspace/persistence/WorkspacePersistenceCoordinatorSession.cpp:620`), and restore has parsing logic for
those fields (`src/workspace/persistence/WorkspacePersistenceCoordinatorSession.cpp:255`). The binary tab tag
schema has no tags for either field (`src/workspace/persistence/WorkspacePersistenceBinaryInternal.h:182`), and
`EncodeEditorTab` / `DecodeEditorTab` never write or read them
(`src/workspace/persistence/WorkspacePersistenceBinaryInternal.h:578`).

Impact: staged/unstaged/combined working-tree review tabs and branch/commit/conflict review modes
can restore with default semantics even though the in-memory persisted structure suggests otherwise.

Recommended fix: add binary tags and decode cases for both fields, with compatibility for older
records. Add a session round-trip test for staged and branch compare tabs.

### Medium - Restored compare metadata can be parsed and then dropped during tab rebuild

Status: confirmed by code inspection.

Restore parses `compare_review_mode` and `compare_staging_view` into a local compare state
(`src/workspace/persistence/WorkspacePersistenceCoordinatorSession.cpp:255`), but the actual compare-tab rebuild
path has separate construction defaults (`src/workspace/shell/WorkspaceShellCompare.cpp:119`,
`src/workspace/shell/WorkspaceShellCompare.cpp:477`). The persistence code needs a regression that proves
the rebuilt tab, not just the intermediate state, preserves review mode and staging view.

Recommended fix: make restored compare tab construction consume the persisted compare state as the
single source of truth.

### Medium - LSP `didOpen` can still run synchronously on tab activation paths

Status: confirmed by code inspection and architecture-lint gap.

The intended contract says `textDocument/didOpen` and `didChange` must not be sent synchronously on
the `ActivateTab` path. The lint scans narrowly for direct `didOpen` / `didChange` dispatch in the
tab coordinator (`tests/architecture/WorkspaceCoordinatorArchitectureRules.cpp:34`), but activation
still reaches buffer-open hooks (`src/workspace/coordinators/WorkspaceTabCoordinator.cpp:367`) and then plugin/LSP
open notification code (`src/workspace/shell/WorkspaceShellPlugins.cpp:905`). The LSP service serializes
the document for open notifications (`src/workspace/lsp/LspService.cpp:799`).

Impact: activating a large restored tab can still pay full-buffer serialization / JSON construction
on the shell path, and the current invariant can miss it because the call is indirect.

Recommended fix: make tab activation post LSP hydration asynchronously through the existing wake
path, then broaden the lint to scan the actual operation-hook names and not only direct protocol
method strings.

## Editor, Compare, Merge, And Render

### High - Raw Git conflict blocks can collapse to base and be marked resolved

Status: confirmed by explorer review and spot-checked in code.

`SelectGitConflictBlock` falls through to `Base` when the current and incoming marker sections both
match the merge model and the base section also matches (`src/workspace/shell/WorkspaceShellMergeState.cpp:66`,
`:89`). That selected choice is then accepted as a valid hint
(`src/workspace/shell/WorkspaceShellMergeState.cpp:359`) and stored as resolved
(`src/workspace/shell/WorkspaceShellMergeState.cpp:409`).

Impact: opening an unedited raw Git conflict block can silently collapse the result to base content
and mark the conflict resolved. That is a data-loss-class merge bug.

Recommended fix: raw marker blocks that exactly match the model should remain unresolved/raw unless
the user explicitly chooses a side or marks the conflict resolved. Add a regression that opens a
normal conflict-marker file and asserts the result does not auto-select base or mark the conflict
resolved.

### High - Empty post-context can make arbitrary merge-result text look resolved

Status: confirmed by explorer review and spot-checked in code.

`FindSequence` returns `start` for an empty needle (`src/workspace/shell/WorkspaceShellMergeState.cpp:149`).
`RefreshMergeTabDerivedState` uses that helper to infer committed result spans when a hunk does not
match a known choice (`src/workspace/shell/WorkspaceShellMergeState.cpp:387`). For the last conflict or
adjacent hunks with no base context between them, the empty post-context can produce a zero-length
span, infer a choice, and mark the conflict valid/resolved while dropping unmatched manual result
text from conflict tracking.

Recommended fix: distinguish "no context available" from "empty sequence matched" in result-span
inference. Add cases for final-conflict manual edits and adjacent conflicts with no intervening base
context.

### High - Delete/modify merge conflicts cannot naturally resolve to deletion

Status: confirmed by explorer review and spot-checked in code.

`MarkMergeResolved` infers existence from `!merge_tab->result_viewport.lines().empty()`
(`src/workspace/coordinators/WorkspaceCompareInteractionCoordinator.cpp:581`). Text buffers are normalized to at
least one line, so `requires_existence_choice` conflicts cannot naturally produce
`result_should_exist = false`. Validation then compares the expected existence with the filesystem
state (`src/workspace/git/MergeResultValidation.cpp:91`).

Impact: delete-side resolutions for modify/delete conflicts can be blocked or misclassified unless
the user manipulates the file outside the normalized editor buffer.

Recommended fix: represent delete resolution as explicit merge state, not as "the editor has zero
lines." Add a modify/delete conflict test that resolves by deletion and marks resolved from inside
the merge UI.

### Medium - Merge source accept-button geometry can drift from rendered rows

Status: confirmed by explorer review.

Source accept-button Y positions are based on `interaction.result.text.scroll_line`
(`src/workspace/shell/WorkspaceShellMerge.cpp:368`, `src/workspace/WorkspaceLayout.cpp:559`), while source
panes render from `merge_tab.scroll_row`. If the source panes have more rows than the result pane,
the result viewport scroll can be clamped lower than the source scroll, so buttons and hover targets
can land on the wrong source rows.

Recommended fix: compute all merge row geometry from one merge scroll model, with source/result
clamping handled per pane only at final draw/hit-test.

### Medium - Zero-length source conflict spans disappear from source hit-testing

Status: confirmed by explorer review.

Source conflict lookup ignores spans where `start == end`
(`src/workspace/WorkspaceLayoutMergeAndChrome.cpp:202`,
`src/workspace/WorkspaceLayoutMergeAndChrome.cpp:210`). Other result-side rectangle code expands
zero-length spans, but source lookup does not. Insert/delete-side conflicts therefore lose source
hover, click, or tint behavior on that side.

Recommended fix: normalize zero-length source spans the same way result spans are normalized before
hit-testing and decoration.

### Medium - `show_raw_markers` appears to be dead UI state

Status: confirmed by explorer review.

`ToggleMergeRawMarkers` flips `merge_tab->show_raw_markers`
(`src/workspace/coordinators/WorkspaceCompareInteractionCoordinator.cpp:532`), and the field exists on
`MergeTabState` (`src/workspace/state/WorkspaceTabState.h:159`), but the explorer found no render, save,
validation, or rebuild consumer. The command therefore appears to redraw without changing what the
user sees or saves.

Recommended fix: either implement raw-marker rendering/save semantics or remove the command/state.

### Medium - Render translation units still materialize strings in hot paths

Status: confirmed by code inspection.

The render-path invariant says `WorkspaceShellRender*.cpp` should consume render view models and
avoid string assembly in per-frame rendering. Several render files still build strings or path
strings directly:

- `src/workspace/render/WorkspaceShellRenderOverlay.cpp:145` builds a command fallback label.
- `src/workspace/render/WorkspaceShellRenderOverlay.cpp:183` and `:192` build project-search fallback and
  progress strings.
- `src/workspace/render/WorkspaceShellRenderOverlay.cpp:214` builds per-row project-search labels and
  `:224` calls `relative_path.string()`.
- `src/workspace/render/WorkspaceShellRenderOverlay.cpp:241` builds picker row vectors each render.
- `src/workspace/render/WorkspaceShellRenderOverlay.cpp:288` concatenates completion labels.
- `src/workspace/render/WorkspaceShellRenderOverlay.cpp:413` builds buffer-search count strings.
- `src/workspace/render/WorkspaceShellRenderSidebar.cpp:41` has a render-local label builder.
- `src/workspace/render/WorkspaceShellRenderSidebar.cpp:336` calls `relative_path.string()` for search
  results.
- `src/workspace/render/WorkspaceShellRenderSidebar.cpp:591` and `:692` build git row labels.
- `src/workspace/render/WorkspaceShellRenderFrame.cpp:556` builds a URI from
  `viewport.path().generic_string()` while rendering review comment markers.
- `src/workspace/render/WorkspaceShellRenderMerge.cpp:497` materializes merge preview lines while
  rendering.

The existing render lint checks only a few patterns such as `std::to_string`, `std::format`, and
some `std::string(...) +` patterns (`tests/architecture/WorkspaceServiceArchitectureRules.cpp:39`),
and explicitly leaves overlay/sidebar allocation scope incomplete (`tests/architecture/WorkspaceServiceArchitectureRules.cpp:582`).

Impact: repeated redraws for search overlays, completion lists, git status, review comments, and
merge previews can allocate and reformat labels every frame.

Recommended fix: move these labels into `RenderViewModelBuilder` or stable per-state caches, keep
render TUs on `std::string_view` / scratch rows, and harden the lint around `.string()`,
`generic_string()`, vector construction, and local string builders in render TUs.

### Medium - Merge-result edit tracking snapshots whole documents before common edits

Status: confirmed by code inspection.

Ordinary `TextViewport` edit paths are mostly range-scoped, but merge result side-effect tracking
still takes whole-document snapshots before common edits:

- typing into a merge result: `src/workspace/coordinators/WorkspaceTextInputCoordinator.cpp:338`
- key edits in a merge result: `src/workspace/coordinators/WorkspaceKeyInputCoordinatorEditor.cpp:277`
- undo/redo/cut/insert flows: `src/workspace/actions/WorkspaceActionServices.cpp:625`,
  `src/workspace/actions/WorkspaceActionServices.cpp:698`, `src/workspace/actions/WorkspaceActionServices.cpp:796`,
  `src/workspace/actions/WorkspaceActionServices.cpp:864`
- assist/snippet edits: `src/workspace/services/AssistService.cpp:257`

Impact: editing a large merge result can copy the whole buffer on keystrokes or common commands,
which violates the spirit of the range-based edit invariant even if it does not copy
`document_->lines` inside `TextViewport` itself.

Recommended fix: track the affected ranges or revision deltas for merge-result side effects instead
of pre-copying the whole document. Add a perf regression using a large merge output with sparse
edits.

### Medium - Merge preview choices are recomputed in render

Status: confirmed by code inspection.

`WorkspaceShellRenderMerge.cpp:497` calls `compare::MergeChoiceLines(...)` and stores a
`std::vector<std::string>` during render. This duplicates merge-choice computation and allocation in
the frame path.

Recommended fix: precompute visible preview lines in the merge view model or keep a stable scratch
buffer owned outside the render TU.

### Speed - Merge rendering and hit-testing scan conflicts per visible row

Status: confirmed by explorer review.

Merge rendering and source-hit lookup repeatedly scan conflict spans for visible rows
(`src/workspace/render/WorkspaceShellRenderMerge.cpp:233`,
`src/workspace/WorkspaceLayoutMergeAndChrome.cpp:202`,
`src/workspace/WorkspaceLayoutMergeAndChrome.cpp:217`). With many conflicts, scrolling and redraw
work becomes `visible_rows * conflict_count`.

Recommended fix: exploit the sorted conflict spans with a cursor, interval index, or prebuilt
visible-row map.

### Speed - Merge syntax tokenization starts from row zero even after deep jumps

Status: confirmed by explorer review.

`PopulateMergeSyntaxTokensForWindow` advances tokenization in 256-line chunks from the previous
tokenized prefix (`src/workspace/shell/WorkspaceShellMergeState.cpp:460`, `:467`, `:483`). Jumping deep
into a large merge file can spend many frames tokenizing off-screen prefix rows while visible rows
remain untokenized.

Recommended fix: add a visible-window-first tokenization path or resumable syntax checkpoints so
deep jumps prioritize visible rows.

### Speed - Merge result-span inference repeatedly allocates and scans

Status: confirmed by explorer review.

Result-span inference materializes candidate choice lines and post-context vectors while searching
for matching spans (`src/workspace/shell/WorkspaceShellMergeState.cpp:365`, `:371`, `:387`). In large
merge outputs with many conflicts and ambiguous context, this can approach quadratic behavior.

Recommended fix: avoid vector copies for candidate and context comparisons, and pre-index stable
context anchors where possible.

### Speed - Grouped undo fallback still snapshots a full `TextBuffer`

Status: confirmed by explorer review.

The grouped edit fallback in `src/editor/TextViewportUndoHistory.cpp:84` and `:142` still snapshots
a whole `TextBuffer` when grouped entries stop merging. This is outside the direct
`document_->lines` copy lint but can still copy large buffers on non-const edit paths.

Recommended fix: make grouped fallback range-based or cap the fallback so large documents do not
silently take whole-buffer snapshots.

## Search, Git, Project, And External Processes

### High - Native file-index watchers miss subtree creates, moves, and deletes

Status: confirmed by explorer review.

The Linux watcher adds a recursive watch for a newly created or moved-in directory but reports no
file changes for files already inside that directory (`src/platform/FileIndexWatcher.cpp:544`,
`:572`). Directory delete or move-self handling can remove the watch without emitting child-file
deletions. `FileIndex::ApplyBatch` only applies exact file path updates
(`src/project/FileIndex.cpp:196`), so the maintained index can stay stale until a full rescan.

Impact: project search, file finder, and project-tree views can miss files that arrive inside a new
subtree, or keep showing files deleted by a subtree move/delete.

Recommended fix: directory create/move-in should enqueue a bounded subtree scan batch, and directory
delete/move-out should enqueue removals for known indexed children. Add Linux watcher tests for a
pre-populated directory moved into the project and a watched subtree removed from the project.

### High - POSIX subprocess timeout can be bypassed after stdio closes

Status: confirmed by explorer review.

`PumpChildIo` can return non-timeout once stdout/stderr capture fds are drained
(`src/platform/Subprocess.cpp:154`, `:200`). The caller then performs blocking `waitpid(..., 0)`
(`src/platform/Subprocess.cpp:542`). A child that closes or redirects stdout/stderr and then hangs
can ignore `timeout_ms`.

Impact: any caller relying on subprocess timeouts, including git, formatting, tools, and plugin
process work, can hang despite passing a timeout.

Recommended fix: keep deadline enforcement active until process exit, not just until capture pipes
close. Add a subprocess test whose child closes stdio and sleeps beyond the timeout.

### High - Windows subprocess capture cap can deadlock

Status: confirmed by explorer review.

The Windows pipe reader stops reading once `kMaxCaptureBytes` is reached
(`src/platform/Subprocess.cpp:422`), while the parent starts reader threads
(`src/platform/Subprocess.cpp:641`) and waits for the process with `INFINITE`
(`src/platform/Subprocess.cpp:654`). A child that writes beyond the capture cap can block on a full
pipe while the parent waits forever.

Impact: Windows support is not primary yet, but the platform layer can deadlock under large-output
subprocesses instead of returning a capped result.

Recommended fix: continue draining and discarding after the capture cap, or terminate on cap/timeout
with a clear result. Add a Windows-platform subprocess regression when that test environment is
available.

### Medium - Cross-device moves can partially copy and duplicate project state

Status: confirmed by explorer review.

`platform::MovePath` falls back from rename to copy-then-remove on failure
(`src/platform/FsOps.cpp:77`). Project rename uses that path
(`src/project/FileOperationService.cpp:95`), and trash operations do as well
(`src/platform/Trash.cpp:109`). If the copy partially succeeds or the remove fails, callers get
`false` after filesystem state has already changed.

Impact: project file moves and trash operations can leave duplicated trees or partial destinations,
which is especially risky for large directories.

Recommended fix: expose cross-device move as a multi-step operation with explicit partial-failure
cleanup and user-visible errors. Prefer temp destinations and rollback where possible.

### Medium - Git porcelain v2 parser corrupts paths that begin with spaces

Status: confirmed by explorer review.

`PathAfterLeadingTokens` skips all spaces before the path
(`src/project/GitPorcelainV2Parser.cpp:18`), and porcelain v2 parsing uses that helper for path
fields (`src/project/GitPorcelainV2Parser.cpp:163`). Existing tests cover internal spaces but not
leading spaces (`tests/GitRepositoryStateTests.cpp:80`).

Impact: a tracked file named ` leading.cpp` can be reported as `leading.cpp`, causing status, diff,
stage, or discard actions to target the wrong path.

Recommended fix: parse exactly the porcelain delimiter structure instead of trimming path-leading
spaces. Add fixtures for leading-space paths and quoted/renamed records.

### Medium - Commit changed-file collection is newline-unsafe

Status: confirmed by explorer review.

Commit compare uses `git diff-tree --name-only -r` and parses the output by line
(`src/project/GitCompareService.cpp:335`). Workspace review paths consume those file lists
(`src/workspace/coordinators/WorkspaceDiffTabCoordinator.cpp:129`,
`src/workspace/git/ReviewSessionCoordinator.cpp:185`). Git paths may contain newlines, while nearby
working-tree code already uses `-z`.

Impact: commit review sessions can split one file into bogus entries and then open/diff/stage the
wrong paths.

Recommended fix: switch commit file collection to `-z` output and parse NUL-delimited records.

### Medium - Commit staged-summary parsing is newline-unsafe

Status: confirmed by explorer review.

`git diff --cached --numstat` parsing in `CommitWorkflowChecks` is newline-delimited
(`src/project/CommitWorkflowChecks.cpp:68`, `:84`, `:190`). Paths with newlines corrupt the staged
summary and can suppress or misreport the partial-staging warning.

Recommended fix: use `git diff --cached --numstat -z` and parse records according to Git's NUL
format.

### Low - Terminal startup can drop immediate query replies

Status: explorer-identified API risk.

`TerminalSession::Start` wires backend callbacks before assigning `backend_`
(`src/terminal/TerminalSession.cpp:106`, `:147`). If backend startup immediately parses a terminal
query and flushes a reply, `SendBytes()` can see no backend and drop it
(`src/terminal/TerminalSession.cpp:338`).

Impact: rare startup races can make early terminal capability probes mis-detect support.

Recommended fix: assign the backend before starting callbacks, or queue replies until startup
publishes the backend pointer.

### Low - Terminal backend pointer lifetime is fragile under concurrent stop

Status: explorer-identified API risk.

`Resize()` and `SendBytes()` take a raw backend pointer under lock, unlock, then call it
(`src/terminal/TerminalSession.cpp:211`, `:317`, `:338`). `Stop()` can concurrently move and destroy
the backend. Current workspace usage appears mostly UI-threaded, so this is more of a boundary risk
than a confirmed shipped crash.

Recommended fix: serialize terminal backend operations on one owner thread or hold a shared/lifetime
token across unlocked calls.

### Medium - Project replace-all can partially apply changes without surfacing an error

Status: confirmed by code inspection.

`ReplaceAllProjectSearchMatches` validates and buffers matching files before writing, including
aggregate-size protection (`src/workspace/shell/WorkspaceShellProjectSearch.cpp:371`). Once writes begin,
each file is opened with truncation (`src/workspace/shell/WorkspaceShellProjectSearch.cpp:428`). If opening
or writing a later file fails, the function returns immediately (`src/workspace/shell/WorkspaceShellProjectSearch.cpp:429`,
`:433`) after earlier files were already overwritten. It does not set an error message before
returning and does not refresh search state.

Impact: replace-in-project can leave a mixed project state while the UI gives little indication of
which files changed. This is a correctness and trust issue more than a pure UX issue.

Recommended fix: write each changed file through a temp-file-and-rename path, collect failures, and
surface a precise partial-apply error. If full atomicity across files is not practical, make the
operation explicit about partial success and refresh search results.

### Medium - Synchronous formatter and tool hash subprocesses bypass the workspace invariant

Status: confirmed by code inspection.

The architecture lint rejects direct `platform::RunSubprocess(` in `src/workspace/*.cpp`
(`tests/architecture/WorkspaceCoordinatorArchitectureRules.cpp:181`), but workspace code can still
block by calling the project wrapper:

- format-on-save calls `project::RunSubprocess` from
  `src/workspace/coordinators/WorkspaceTabCoordinatorShellBridge.cpp:261`
- tool SHA256 verification calls `project::RunSubprocess` from
  `src/workspace/WorkspaceToolDownloader.cpp:96`, `:102`, and `:108`

Impact: save and tool-install flows can block the shell thread. On Windows this is worse because the
subprocess timeout is not honored by the Windows implementation.

Recommended fix: route these through `ProjectBackgroundExecutor` and broaden the lint to reject
workspace calls to both `platform::RunSubprocess` and `project::RunSubprocess` except in approved
executor seams.

### Low - File watcher tests still rely on fixed sleeps

Status: confirmed by code inspection.

`tests/FileIndexWatcherTests.cpp:306` uses a fixed `sleep_for(200ms)` before asserting no callback
after `Unwatch`. This can pass locally but fail under scheduling pressure or slower CI.

Recommended fix: replace the fixed sleep with an event-driven barrier or bounded polling helper that
observes the watcher thread state.

## Plugin And Lua Runtime

### Critical - Project switch can tear down Lua state without draining the plugin worker

Status: confirmed by explorer review and spot-checked in code.

Project switch uses `WorkspacePluginRuntime::ShutdownHost()`, which calls `plugin_host_.Shutdown()`
directly (`src/workspace/WorkspacePluginRuntime.cpp:156`). Full runtime shutdown first joins the
plugin worker and clears the host worker pointer (`src/workspace/WorkspacePluginRuntime.cpp:160`),
but the project-switch shutdown path does not. `PluginHost::Shutdown` calls into host teardown
(`src/plugin/PluginHostPublicApi.inc:97`), and lifecycle teardown runs plugin close/shutdown
callbacks and unregisters state (`src/plugin/PluginLifecycleCallbackInterop.cpp:137`).

Impact: queued or running worker jobs can still touch plugin state while the UI thread runs
`on_project_close`, `shutdown`, unregisters callbacks, or closes the Lua state. This violates the
drain-before-teardown invariant and can become a race/use-after-free around Lua state destruction.

Recommended fix: make project-switch shutdown use the same bounded "join/drain worker, detach worker,
then tear down host" sequence as full runtime shutdown. Add a project-switch test with a delayed
worker job and assert teardown waits or cancels safely.

### High - Async reload completion can apply to the wrong project

Status: confirmed by explorer review.

`ReloadPluginsForCurrentProject` uses `reload_plugins_invocation_count_` as the only staleness guard
for async reload completions (`src/workspace/shell/WorkspaceShellPlugins.cpp:669`, `:679`, `:682`).
Project switching or reactivating an already initialized project does not necessarily increment that
counter, and reactivation intentionally avoids plugin reload
(`src/workspace/shell/WorkspaceShellPlugins.cpp:761`). A delayed completion from the previous project can
therefore pass the generation check and run reload consumption against the newly active project.

Impact: plugin registry snapshots, sidebars, language servers, syntax state, or redraw work can be
published into the wrong project after a switch/reactivation.

Recommended fix: include a project epoch/root identity in reload requests and completions, and drop
completion work unless both reload generation and project epoch still match.

### High - Posted plugin host mutations lack project/reload epoch guards

Status: confirmed by explorer review.

Most posted plugin callbacks mutate `context_.current_project_state` without checking that the
originating project is still active. Examples include diagnostics/decorations/surfaces in
`src/workspace/shell/WorkspaceShellPlugins.cpp:389` and `:416`, and callback posting in
`src/plugin/PluginHostCallbacks.cpp:48`, `:70`, and `:94`. The edit path has a staleness guard
(`src/plugin/PluginHostCallbacks.cpp:130`), but the other mutation paths do not.

Impact: async plugin results that complete after a project switch can publish diagnostics,
decorations, notifications, sidebar data, or rasterized surfaces into the wrong project.

Recommended fix: carry a project/reload epoch through all plugin-to-host posted mutations and drop
stale callbacks before they touch workspace state.

### High - Plugin filesystem containment can escape through existing symlink parents

Status: confirmed by explorer review.

Plugin path containment allows missing targets after lexical validation
(`src/plugin/PluginPathInterop.cpp:60`, `:63`). `ctx.files.write_text(...)` resolves a runtime path
and then writes atomically (`src/plugin/PluginWorkspaceInterop.cpp:154`,
`src/util/TextFileIO.cpp:134`, `:137`). For a path like `link/new.txt` where `project/link` is a
symlink to outside the project, the missing target can pass lexical containment and then be created
beside the followed symlink target.

Impact: a plugin with project-scoped write capability can write outside the project/data root via a
symlinked existing parent.

Recommended fix: canonicalize the deepest existing parent before allowing creation of a missing
child, reject symlink escapes, and add read/write containment tests for symlink parents.

### High - Raw `lua_State*` has leaked across plugin modules despite the opacity contract

Status: confirmed by code inspection.

The durable contract says `lua_State*` should be confined to `plugin/LuaRuntime.{h,cpp}`. Current
plugin runtime types and interop files expose the raw pointer broadly, including
`src/plugin/PluginHostRuntimeTypes.h:35`, `:55`, `:63`, `:74`, `:83`, `:94`, `:104`, `:126`, `:136`,
`:146`, `:157`, `:166`, and `:177`. The current plugin architecture test only protects selected
editor-facing files from Lua includes (`tests/architecture/PluginArchitectureRules.cpp:93`), not the
full opacity invariant.

Impact: plugin extension surfaces can keep adding direct Lua stack manipulation, making lifecycle,
error handling, and async worker boundaries harder to reason about.

Recommended fix: introduce an opaque runtime handle / typed call facade and move stack operations
behind `LuaRuntime`. Add a hard-fail lint over `src/plugin/**` with a small allowlist for
`LuaRuntime.h` and `LuaRuntime.cpp`.

### Medium - Synchronous plugin APIs can wait unbounded on the UI thread

Status: confirmed by code inspection.

`PluginHostInternal::RunOnWorkerBlocking` posts to the worker and then calls `finished.wait()`
without a deadline (`src/plugin/PluginHostInternal.h:224`, `:249`). The comment acknowledges the
wait is unbounded and relies on per-call Lua watchdogs (`src/plugin/PluginHostInternal.h:240`).
Many public synchronous APIs use it, including reload (`src/plugin/PluginHostPublicApi.inc:42`),
command execution (`src/plugin/PluginHostPublicApi.inc:287`), code actions
(`src/plugin/PluginHostPublicApi.inc:936`), and several provider queries.

Impact: a long-running or wedged worker task can freeze command palette actions, code actions, SCM
queries, auth calls, and reload. The watchdog does not bound queue wait time, subprocess wait time,
or non-Lua work inside the worker.

Recommended fix: convert UI-callable plugin actions to async or bounded waits with safe ownership for
late completions. Keep blocking only for startup/shutdown paths that have explicit bounded drains.

### Medium - Plugin reload and shutdown teardown needs a stronger drain invariant

Status: confirmed architecture risk.

`RunReloadLoad` calls `TearDownPlugins()` directly while switching projects
(`src/plugin/PluginHostInternal.h:298`, `:303`). Public shutdown also calls `impl_->TearDownPlugins()`
directly (`src/plugin/PluginHostPublicApi.inc:108`). The policy requires plugin reload/shutdown to
drain async process workers before plugin teardown using the bounded drain seam. The direct teardown
calls make that guarantee hard to audit and easy to regress.

Recommended fix: centralize reload/shutdown teardown through one explicit "bounded drain then
teardown" helper and lint against direct teardown from public/reload paths.

### Medium - Some plugin API errors can Lua-longjmp across non-trivial C++ locals

Status: confirmed by explorer review.

Several plugin API paths call `luaL_check*` while non-trivial C++ locals are live in caller frames.
Examples include `RegisterTableContribution` with a live `std::string error_message`
(`src/plugin/PluginHostLuaApi.inc:141`), shorthand SCM/auth provider registration
(`src/plugin/PluginHostLuaApi.inc:616`, `:647`), diagnostics/decorations clear with live
`std::optional<std::filesystem::path>` (`src/plugin/PluginHostLuaApi.inc:811`, `:838`), and
presentation parser checks (`src/plugin/PluginPresentationRegistrationParsers.cpp:75`, `:138`).

Impact: malformed plugin input can invoke Lua's longjmp error path and bypass C++ destructors,
contrary to the local Lua error-safety guidance.

Recommended fix: replace `luaL_check*` in these paths with non-longjmp validation helpers that
return pending errors, or ensure all `luaL_check*` calls happen before non-trivial locals exist.

### Medium - Provider result harvesting trusts raw Lua array length

Status: confirmed by explorer review.

Several provider result parsers use `lua_rawlen()` as the loop bound after `PCall`, including
completion/code-action/test/SCM/auth-style harvests in
`src/plugin/PluginProviderQueryInterop.cpp:46`, `:116`, `:129`, `:189`, `:262`, `:331`, `:402`, and
`:479`. A plugin can return a sparse table with a huge border and make the worker spend unbounded
time or build unbounded host vectors after the Lua watchdog has returned.

Recommended fix: route provider arrays through shared capped-array helpers, matching the caps that
already exist for diagnostics and language symbols.

### Medium - Subprocess timeouts are ignored on Windows, including plugin subprocesses

Status: confirmed by code inspection.

Plugin subprocess calls pass a 120 second timeout (`src/plugin/PluginProcessInterop.cpp:27`,
`:205`, `:255`). `platform::SubprocessOptions::timeout_ms` documents that timeout handling is
POSIX-only (`src/platform/Subprocess.h:24`). The Windows implementation waits with
`WaitForSingleObject(process_info.hProcess, INFINITE)` (`src/platform/Subprocess.cpp:654`).

Impact: Windows support is not primary yet, but any plugin subprocess on Windows can hang the worker
indefinitely. The same platform gap affects other subprocess callers that expect timeouts to bound
latency.

Recommended fix: implement Windows timeout handling with `WaitForSingleObject(..., timeout)` and
terminate/collect on timeout, matching the POSIX behavior.

### Low - Plugin lifecycle ref ownership is inconsistent

Status: confirmed by explorer review.

`DestroyPluginState()` unrefs setup/project/open/save/shutdown refs, but the explorer found that
`on_buffer_change_ref`, `on_cursor_move_ref`, `on_selection_change_ref`, and `on_buffer_close_ref`
from `src/plugin/PluginLifecycleLoadInterop.cpp:260`, `:266`, `:272`, and `:278` are not mirrored in
`src/plugin/PluginStateTeardownInterop.cpp:31`. `lua_close` reclaims them in the current teardown
order, so this is not a live leak today, but the ownership model is inconsistent.

Recommended fix: unref all lifecycle refs in the same teardown helper and add a test that loads all
lifecycle callbacks then tears down without relying on final `lua_close`.

## Terminal, Platform, And Event Loop

### Low - Event loop idle strategy looks correct, but keep the invariant covered

Status: checked during audit.

The SDL loop uses `IdleHint` to choose between `SDL_PollEvent`,
`SDL_WaitEventTimeout`, and `SDL_WaitEvent` (`src/app/Application.cpp:199`,
`src/app/IdleWaitStrategy.h:26`). The follow-up risk is coverage: future event-loop edits should
keep a lint or targeted test around the no-zero-delay-idle-spin invariant.

### Low - Polling file watcher fallback trades latency for periodic wakeups

Status: confirmed by code inspection.

`src/platform/FileIndexWatcher.cpp:652`, `:908`, and `:1200` use a fixed polling fallback interval.
This is not a correctness bug by itself, but it is a speed/CPU tradeoff that should be visible in
idle-soak perf scenarios once the perf harness records wakeups and CPU.

Recommended fix: keep the fallback, but include watcher idle scenarios in perf baselines with CPU
and wakeup metrics.

## Tests, Performance Gates, Fuzzing, And Architecture Lints

### High - Documented sanitizer/fuzz/perf gates are not actually enforced

Status: confirmed by code and docs.

The specs describe sanitizer, fuzz, and perf-runner gates as durable merge safeguards
(`openspec/specs/bug-detection-tooling/spec.md:12`,
`openspec/specs/performance-harness/spec.md:62`). `dev-docs/project/active-work.md:695` still says
native Windows/macOS, CI automation, and perf-runner integration are deferred/manual. The
`tools/run-checks.sh` wrapper has targets for tests and sanitizers but no fuzz target
(`tools/run-checks.sh:125`).

Impact: the branch can be green locally while missing the gates the specs say should protect `main`.

Recommended fix: either wire the gates or revise the specs and active-work document to the actual
merge policy. Prefer wiring `run-checks.sh fuzz` and a perf baseline mode rather than weakening the
contract.

### High - `microide_perf_tests` is smoke-only and suppresses baseline failures

Status: confirmed by code inspection.

CTest runs perf tests as `microide_perf --smoke --iterations=1` (`CMakeLists.txt:1105`). In smoke
mode, missing baselines do not fail (`tests/perf/PerfMain.cpp:1369`) and failed baseline comparisons
are ignored (`tests/perf/PerfMain.cpp:1377`).

Impact: the default green test run can miss the exact regressions the performance specs require it
to catch.

Recommended fix: split smoke from gate mode. Keep smoke fast for local sanity, but add a required
baseline-check target for merge readiness.

### High - Perf baselines capture only wall time and allocation counts

Status: confirmed by code inspection.

`MetricSet` stores wall-clock and allocation metrics only (`tests/perf/PerfHarness.h:32`). Baseline
load/compare code therefore cannot enforce CPU, RSS, wakeups, redraw counts, background task counts,
or frame-tier counters (`tests/perf/Baseline.cpp:49`, `:91`). The specs require those dimensions for
idle and redraw-sensitive scenarios (`openspec/specs/performance-harness/spec.md:49`,
`openspec/specs/performance-harness/spec.md:162`).

Impact: idle CPU, redraw churn, and background-work regressions can pass as long as wall time and
allocation counts stay inside bounds.

Recommended fix: extend `MetricSet`, baselines, and perf fixtures to record the documented counters,
then update baseline JSON files.

### Medium - Allocation assertions in perf scenarios are ignored

Status: confirmed by code inspection.

`ScenarioContext::AssertNoAllocationsDuringDraw` returns `bool`
(`tests/perf/PerfHarness.h:94`), but scenarios cast it to void at
`tests/perf/PerfMain.cpp:428`, `:441`, and `:830`.

Impact: draw-allocation regressions can be measured and then silently ignored.

Recommended fix: throw or mark the scenario failed when the assertion returns false, and print the
captured error string.

### Medium - `std::sto*` invariant is warning-only and misses tools drift

Status: confirmed by code inspection.

The repo policy says no `try`/`catch` around `std::stoi`, `std::stoll`, `std::stoull`, `std::stof`,
or `std::stod` in `src/`, `tests/`, or `tools/`. The architecture rule scans for those tokens but
does not hard-fail in the same way as the main invariants. Drift exists in tools:

- `tools/CompareBench.cpp:450`
- `tools/ProjectSearchBench.cpp:143`

Perf tests also use `std::stoull` directly in `/proc` parsing (`tests/perf/PerfMain.cpp:118`).

Recommended fix: replace with `util::Parse*` helpers where appropriate and make the invariant
hard-fail across `src`, `tests`, and `tools`, with narrow allowlists only where justified.

### Medium - Fuzz targets are buildable but not part of the standard wrapper

Status: confirmed by code inspection.

Fuzz targets exist under the `MICROIDE_FUZZ` CMake option (`CMakeLists.txt:922`), but
`tools/run-checks.sh` has no `fuzz` target. Several corpora are seedless except `.gitkeep`,
including persisted-record, blame parser, plugin display-list, and surface raster fuzz corpora.

Impact: parser and persistence regressions can land without exercising the fuzz targets the testing
rules call out.

Recommended fix: add `tools/run-checks.sh fuzz`, seed the empty corpora with representative current
records, and run affected fuzzers when persistence, parser, regex, blame, or plugin display-list
code changes.

### Medium - Architecture lints do not cover several durable policy contracts

Status: confirmed by code inspection.

The current lint set catches many important invariants, but this audit found gaps:

- LSP async-open detection is too narrow and can miss indirect activation paths.
- Render allocation checks miss `.string()`, `generic_string()`, local vectors, and several
  render-local label builders.
- Plugin `lua_State*` opacity is not hard-failed across `src/plugin`.
- Workspace subprocess checks catch `platform::RunSubprocess` but not wrapper calls that still block
  the workspace path.
- Plugin reload/shutdown drain sequencing is policy-only and not linted.

Recommended fix: promote these to hard-fail architecture tests with tight allowlists and small
fixture snippets in `tests/ArchitectureInvariantsTests.cpp`.

## Deduplication And Technical Debt

### Merge edit side-effect capture is duplicated across coordinators

The same "snapshot merge result, run edit, compare/apply side effects" pattern appears in text input,
key input, action services, and assist/snippet code. That makes it easy for future edit paths to miss
merge bookkeeping or reintroduce whole-document copies. Extract a narrow merge-result edit transaction
helper that captures only changed ranges.

### Render model ownership is still split

`RenderViewModelBuilder` exists, but overlay/sidebar/frame/merge render files still compute labels
and derive data. This keeps render CPU unpredictable and forces lint to chase more syntax patterns.
Finish the migration by making render TUs consume prepared labels and numeric counters only.

### Plugin runtime boundaries are too porous

The plugin host is split across focused files, but raw Lua state travels through shared runtime
types and interop modules. That is a boundary smell: extension surfaces should express commands,
queries, providers, and data conversions through typed runtime APIs, not direct stack access.

### Subprocess execution has too many foreground escape hatches

Workspace code can bypass main-thread subprocess policy through `project::RunSubprocess`, while
platform timeout semantics differ by OS. Consolidate subprocess policy around background executor
entry points, and make timeout behavior portable at the platform layer.

### Perf and docs disagree about what protects merges

The specs describe a richer performance and fuzz gate than the checked local path enforces. This is
split-brain documentation and should be resolved before relying on "tests green" as a merge signal.

### Large orchestration files remain pressure points

Several files are still broad enough to hide cross-subsystem coupling:

- `src/workspace/shell/WorkspaceShellPlugins.cpp` is over 1,600 lines.
- `src/workspace/render/RenderViewModelBuilder.cpp` is over 1,500 lines.
- `src/workspace/services/AssistService.cpp` is over 1,500 lines.
- `src/workspace/shell/WorkspaceShellCursor.cpp` is over 1,300 lines.
- `src/workspace/shell/WorkspaceShellRedraw.cpp` is over 1,100 lines.
- `src/workspace/registries/WorkspaceSettingsRegistry.cpp` is over 1,100 lines.
- `src/workspace/WorkspaceLayout.cpp` is over 1,000 lines.

These are not bugs by themselves, but they are the files most likely to re-grow hidden state and
duplicate workflow logic. Future fixes in these areas should bias toward extracting typed services or
view-model builders rather than adding another local branch.
