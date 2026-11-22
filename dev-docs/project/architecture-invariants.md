# Architecture Invariants — the full record

The compact list agents read is `CLAUDE.md` § Hard Architectural Invariants: one
line per rule and the check that enforces it. This file keeps each rule's full
reasoning -- the bug that made it a rule, the earlier lint that passed while blind
-- moved here verbatim on 2026-11-14 from `CLAUDE.md` and from `AGENTS.md`
§ Do-Not-Regress Patterns, which had grown to repeat each other. Most rules are
enforced by `tests/ArchitectureInvariantsTests.cpp` and `tests/architecture/`
(a `Check…` function per rule, with fixtures both ways in
`tests/architecture/ArchitectureRuleFixtures.cpp`); read this before changing or
relaxing one.

## From CLAUDE.md

Several patterns were intentionally removed by the 2026-04-29 `comprehensive-tech-debt-cleanup` change. Do not reintroduce them. The architectural-lint test (`tests/ArchitectureInvariantsTests.cpp`) hard-fails on the load-bearing ones; the policy ones are reviewer-enforced.

- Workspace coordinator constructors take service-interface references, never `WorkspaceShell&` or `WorkspaceShell*`. Services live alongside the shell (`EditorTabService`, `ProjectCatalogService`, `PromptSurfaceService`, `SidebarService`, `CompareMergeService`, `TerminalPanelService`, `PluginRuntimeService`, `PersistenceService`).
- No `friend class`/`friend struct` in `src/workspace/*`, except the sanctioned `WorkspaceShell::TestAccess` backdoor (now declared unconditionally so the shared `microide_core` object library compiles an ODR-identical `WorkspaceShell` for the production and test binaries; the lint exempts friends naming a `*TestAccess` type).
- Numeric token parsing uses `util/Parse.h` (`ParseInt`, `ParseInt64`, `ParseSize`, `ParseFloat`). No `try`/`catch` around `std::sto*`.
- The shell stays a thin orchestrator, bounded by THREE hard-linted caps, not the two that used to be quoted here: `src/workspace/shell/WorkspaceShell.h` ≤ 400 lines, `src/workspace/shell/WorkspaceShell.cpp` ≤ 600 code lines, and `src/workspace/shell/WorkspaceShellMembers.inc` under a ratcheting code-line cap. The header is ~158 lines only because the class's real declaration surface is that `.inc`, included inside the class body — quoting the 400 on its own reads as a bound on the class and is not one (TD-2026-09-13-293a). All three count CODE lines (comments and blanks are free), and the `.inc` cap ratchets DOWN only: read its current number from `tests/ArchitectureInvariantsTests.cpp`, never from memory. New shell-adjacent behavior lands on a service — the `WorkspaceShell*.cpp` companion count is at its ratchet-only cap of 47, so there is no room for another companion TU either.
- Workspace-state persistence (project state, user config, session restore) routes through `PersistenceService` plus `PersistedRecordReader`/`PersistedRecordWriter`. Do not hand-roll a new text format or open these files directly elsewhere.
- Single-line input surfaces consume `editor/SingleLineEditor` plus `editor/SingleLineKeyHandler`.
- The active editor viewport is owned by the active editor tab; resolve it through `EditorTabService::ActiveViewport()`. Do not reintroduce a shell-level or project-level viewport fallback under any name.
- Plugin host stays decomposed; `lua_State*` lives behind `plugin/LuaRuntime` only, and no `src/plugin/*.cpp` translation unit exceeds 800 lines. Production code outside `src/plugin` must not name `lua_State`, include a Lua header, or include a Lua-exposing plugin header (transitively) — enforced by the hard invariant `CheckLuaStaysBehindPluginBoundary`; the outward-facing surfaces (`PluginHost.h`, `PluginThread.h`) stay Lua-free at the type level. Sole exemption: `src/editor/SyntaxDefinitionLoader.cpp`'s self-contained sandbox state.
- Plugin Lua errors never `longjmp` over live C++ destructors. The project links the C build of Lua, so `luaL_error` unwinds the native stack without running C++ destructors. Raise via `lua_error_util::PushMessage` (`src/plugin/LuaError.h`) then `lua_error` only after every `std::string`/`std::vector`/`std::filesystem::path` local has destructed; delegating TUs return `lua_error_util::kPendingError` and let the thin `.inc` wrapper raise; wrappers bind null-host fallbacks by reference (no temporaries). `luaL_error` is banned in `src/plugin` by the hard invariant `CheckPluginLuaErrorDoesNotLongjmpOverCppLocals` (entry-only `luaL_check*` stays allowed). After touching raise paths in `src/plugin`, rerun the AST scope audit `tools/audit-lua-longjmp.py` (verifies no raise-capable Lua call has a live non-trivially-destructible local in scope; see its docstring for setup).
- Render TUs covered by the lint (`WorkspaceShellRenderFrame`, `WorkspaceShellRenderOverlay`, `WorkspaceShellRenderTextInput`, `WorkspaceShellRenderSidebar`, `WorkspaceShellRenderBottomPanel`, `WorkspaceShellHoverPopup`, `WorkspaceShellHoverTargets`, `DebugPaneRender`) consume view models built by `RenderViewModelBuilder` and do not read `context_.current_project_state` or call `CurrentTextInputSurface(...)`. The debug pane's state-reading geometry/scroll helpers live in `DebugPaneLayout.cpp` (not lint-covered), keeping the render TU view-model-only.
- Repaint damage is derived per frame (openspec `derived-damage`, 2026-11). Before it, every state change named the pixels it changed by hand (`RequestEditorSurfaceRedraw`, `RequestActiveTabRedraw`, ~120 editor-side sites alone), and each surface's prep compared a dozen `painted_*` fields with what it was about to paint. The soak's QA-STALE families were almost all a request some path forgot, and the over-broad ones (an LSP request repainting the window twice, a compare keystroke the whole surface, a click the whole editor) were the cost of being safe. Now a derived surface's `Prepare*Frame` lays out units -- a rect and a fingerprint of everything drawn in it -- and `DerivedDamage::Commit` damages the ones that changed; paint reads only the prepared snapshot (`CheckDerivedSurfacePaintReadsOnlyItsSnapshot`, which also rejects a paint function handed a store or service through its parameters, negative control `paint-handed-a-store`). Changes ask for a frame (`RequestPreparedFrame`), which counts as damage for `EnsureRedraw` fallbacks -- it did not at first, and a click the editor answered with a frame still repainted the whole surface from its button-up fallback. Every surface commits every frame; `DerivedDamage::Reset()` was removed because forgetting the painted tables made the next commit damage nothing. Key completeness is what the redraw verifier and the `*RowDamageFollowsEveryStateTransition` property tests guard; every key gap found so far (a hover card's text, a republished code lens, a gutter that widened under the pane's head, the panel body below its rows, the active tab) was a value paint read and no key folded.
- Render view models own their data (`CheckRenderViewModelsOwnProjectState`, TD-2026-07-17-084/26): no `OverlayState` in `RenderViewModelBuilder.h`, and a `ProjectWorkspaceState*` only in the two documented escape hatches (`FrameSurfaceViewModel`, `SidebarSurfaceViewModel`). The converted render TUs (Overlay, BottomPanel, TextInput, HoverPopup, HoverTargets, DebugPaneRender) must not name either broad state type — labels/rows are precomposed in the builder (owned `label_storage` blob or frame-stable views), and paint-time state mutation moved to frame prep (overlay scroll clamp, bottom-panel tab-strip fill, `PrepareCommitBodyViewportForFrame`).
- Responsive shell surfaces (`LayoutModeService`, `StatusBarService`, `SettingsOverlayService`) stay host-owned and service-backed. Menu overflow, status-bar actions, Settings, and Help/About route through action/service state and `RenderViewModelBuilder`, not plugin-owned rendering or render-TU product logic.
- No legacy persistence symbols (`WorkspacePersistenceLegacyFormat`, `EncodeSessionNodePath`, `DecodeSessionNodePath`, `ParseUserConfigText`, `ParseProjectConfigText`, `ParseProjectSessionText`, `ParseWorkspaceSessionText`) may appear in `src/`, `tests/`, or `tools/`; the legacy importer path is deleted and must not be revived.
- No `platform::RunSubprocess(...)` calls in workspace `.cpp` units; dispatch through `ProjectBackgroundExecutor` to avoid shell-thread stalls.
- Render translation units must not materialize new strings in hot paths (`std::string(...)`, string `+`/`+=`, `to_string`, or `std::format`/`fmt::format`); compute render text in `RenderViewModelBuilder` instead. The compare/merge surface render TUs are named `WorkspaceShellRenderCompare.cpp` / `WorkspaceShellRenderMerge.cpp` (the `WorkspaceShellRender*` prefix is what pulls them under the lint set); do not rename them back to `*CompareRender`/`*MergeRender` or they silently escape the string, single-char, and structural-gate rules. They render one `editor::DecoratedTextRow` per visible row via reused scratch members (`compare_left/right_scratch_row_`, `merge_incoming/current_scratch_row_`) — never a fresh per-row `DecoratedTextRow` — and truncate hot labels through `TruncateLabelView` (allocation-free fits-case), enforced by `CheckCompareMergeRenderUsesScratchRows` and `CheckRenderTuDoesNotMaterializeSingleCharOrPrefixStrings`.
- `Make*Service()`/`Make*Coordinator()` return a reference into the shell-owned `ShellGlueCache`; bind with `auto&` or `Type&`, never by value (a value binding copies the whole coordinator — 17 `std::function`s — per call, undoing the caching). Enforced by `CheckShellGlueIsBoundByReference`. Capturing such a local BY VALUE into a lambda is the same copy per hook even when the local is a reference, and is enforced by `CheckFactoryResultsAreNotCapturedByValue` (which matches both spellings — matching only `auto x = …` once left it scanning nothing).
- Write/start APIs whose dropped result is a silent failure carry `[[nodiscard]]` (`WriteTextFileAtomically`, `TextViewport::Save`, `FileWriteGate::WriteText`, both `Start()`s). A deliberate discard is `(void)` plus the reason. New APIs of this shape get the attribute.
- `FileWriteGate::WriteText` takes `Signature::Skip` by default and only captures a post-write signature on `Signature::Capture`. The editor's save is the one caller that wants it; statting for the others (replace-in-project over N files, the plugin file API, the merge rollback) is a stat per file nobody reads.
- Git availability is THREE states (`project::GitAvailability{Unknown, Repository, NotARepository}`), not a bool. The local source never returns `Unknown`; a remote one is unknown until its first `git/metadata`, and rendering that as "not a repository" is a claim about the user's directory. The status bar shows `scm-unknown` for it.
- `TextViewport` non-const editing paths must not snapshot-copy `document_->lines`; capture affected ranges only for undo/edit operations.
- Overlay dismissal is centralized: no bare `overlay.visible = false` in `src/workspace/*.cpp` outside `WorkspaceShellOverlay.cpp` (canonical `DismissOverlay`) and `WorkspacePersistenceCoordinatorSession.cpp` (full-state restore reset). Hide overlays via `WorkspaceShell::DismissOverlay` or the focus-safe `HideOverlay(state)` helper so keyboard focus never strands on a hidden surface (enforced by `CheckOverlayDismissalIsCentralized`).
- Every descriptor-creating call (`socket`, `accept4`, `open`/`openat`, `pipe2`, `inotify_init1`) must request close-on-exec **on the creating call itself** (`SOCK_CLOEXEC` / `O_CLOEXEC` / `IN_CLOEXEC`), never via a follow-up `fcntl(F_SETFD)` — another thread can fork/exec in the window between the two. The editor spawns terminal shells, LSP servers, DAP adapters, git, and plugin tools, so an unflagged descriptor is inherited by all of them; for the control socket that is a containment hole, not just a leak. Enforced by `CheckDescriptorCreationIsCloseOnExec`, which also bans plain `accept(`, code-masks matches (so `open(`/`socket(` inside string literals are not call sites), and fails loudly if it finds no call sites at all rather than passing vacuously. That rule shipped with `openat?` as its `open` pattern — which matches `opena`/`openat` and never plain `open(` — so it passed green while blind; it now carries negative + positive control fixtures in `tests/architecture/ArchitectureRuleFixtures.cpp`, and any new lint should too.
- Every bool setting read in `src/` through `SettingFlagEnabled`/`SettingEnabled`/`SettingOn`/`SettingBoolIsOn` with a literal dotted key must be declared in `WorkspaceSettingsRegistry`. An unregistered key is silent — it reads as its default forever, stays out of the Settings overlay, and is reachable only by hand-editing a config file with a name found in a source comment. Enforced by `CheckSettingsReadAreRegistered` (code-masked, with the same loud-missing-target guard).

- The editor area's panes are ONE structure in two containers: `EditorSplitTree`'s
  leaves are `ProjectWorkspaceState::editor_groups`, in order (leaf ordinal ==
  group index), so every group-indexed cache, the focused index and the pane rects
  all read that correspondence. A site that inserts or erases a group without a
  matching `InsertLeaf`/`RemoveLeaf`/`Reset` leaves a group with no pane rect, and
  nothing reports it — the group is simply never drawn. Enforced per-TU by
  `CheckEditorGroupsMutateWithTheSplitTree` (a TU that reshapes `editor_groups`
  must call a structural tree method; pairing each individual mutation with its
  tree call stays reviewer-enforced). The cap is `kMaxEditorGroups` (8) and it is
  the capacity of every per-pane inline container plus the tree's node budget
  (2N-1), so raising it cannot leave one behind.

- A compare, a merge, the find widget and the debug toolbar lay out and hit-test
  in the FOCUSED PANE -- `WorkspaceShell::ActiveTabSurface(layout)` (memoized) or
  `FocusedGroupSurface(layout, split, focused)` -- never in `layout.editor_surface`,
  which is the whole editor column. With one group they are the same rect, which is
  how a diff in a split came to paint over every other pane (TD-2026-11-11-439).
  Each pane renders its own front tab, a diff or merge included. Enforced by
  `CheckTabSurfacesLayOutInTheFocusedPane` (no layout entry point of those surfaces
  fed `*.editor_surface`; fixtures both ways).

- Key-hint lists join through `AppendHintSegment`/`JoinHintSegments` in `workspace/WorkspaceUiText.h` (separator `kHintSeparator`, `" · "`). Do not re-declare `AppendHintSegment` in a TU — two files independently grew byte-identical private copies joining on `"  |  "`, which is how the git sidebar's action line and the compare review header ended up spelled differently from every other hint in the app. (The "both are also mirrored into Help/About" once recorded here is not true of the compare one: its strings live only in `CompareTabReview.cpp`, and as of 2026-09-19 that line is not drawn at all — see TD-2026-09-19-296.) Enforced by `CheckHintSegmentsUseTheSharedSeparator`, which has **two** halves. The first targets the redefinition. The second (2026-09-19) targets a string literal that contains `"  |  "` as part of a larger line: scoping to the redefinition alone missed every line that simply typed the wrong separator inline, which was two more — the dirty prompt's hint line, and the git sidebar action line's **no-selection** variant, so the very line the first cleanup was about still changed punctuation as the selection came and went. A **bare** `"  |  "` literal joined at runtime stays legal: that separator is legitimate between unrelated fields (the breadcrumb's `path | left -> right`, the merge status line). That half walks string literals itself rather than matching with a regex and skipping on `BuildCodeMask` — the mask marks literal BODIES as not-code, so the mask-and-skip draft was structurally incapable of firing, and `std::regex` recursed per character and segfaulted on this repo's longer sources.

- The two sides of a conflict, a merge or a compare are **Local** and **Remote** in every user-visible string -- menus, buttons, palette titles and command ids (`git-accept-local`, `remote-resolve <path> local|remote`), toasts, row details, pane captions, compare titles. Never mine / yours / ours / theirs / the host's, nor current / incoming. The same two sides had five names across the app ("Keep Mine" / "Take Host's", "Accept Ours" / "Accept Theirs", "Accept Current (Keep Ours)", "(yours)"), and a row of conflicts could not be read for which button kept what. "Host" stays the remote machine's name ("Open Folder on Host…"). Enforced by `CheckSideNamesAreLocalAndRemote` (literal-walking, short literals only, anchored on the remote conflict row's labels).

- Buffer-content filetype detection goes through `TextViewport::language_id()`, which memoizes on (document identity, content revision, path, syntax-registry revision). Four independent caches for this had accreted — `runtime_syntax::FiletypeMemo` with two instances, plus `LspUiState::language_cache_*`, whose path-only key silently pinned a stale language for content-detected buffers (a shebang with no extension). The content-reading two-argument `DetectFiletype(path, lines)` overload is reserved for that memo; the one-argument path-only form stays legal for callers with no buffer in hand. Enforced by `CheckViewportFiletypeGoesThroughTheViewportMemo` (code-masked, arity-aware, loud on a missing target).

- Same-language editor tabs share ONE `editor::LanguageContractView`, held by `shared_ptr<const>` and built/cached by `WorkspaceLanguageContract::ResolveEditorView` (keyed by language id, dropped whole when the contract revision or any of the three editor toggles changes). Do not give `TextViewport` an owned view back: `ApplyEditorPreferences` walks every open tab, so an owned view means one deep copy of four vectors and three strings per tab per settings change, project activation and session restore. Gated by the `settings_change_many_tabs` perf scenario (TD-2026-08-03-110).

The durable contracts live in `openspec/specs/workspace-architecture/spec.md`, `openspec/specs/persisted-state-format/spec.md`, and `openspec/specs/shared-edit-primitives/spec.md`. The full reasoning is in `AGENTS.md` § Do-Not-Regress Patterns and in the archived change at `openspec/changes/archive/2026-04-29-comprehensive-tech-debt-cleanup/`.

## From AGENTS.md § Do-Not-Regress Patterns

The 2026-04-29 `comprehensive-tech-debt-cleanup` (archived under
`openspec/changes/archive/2026-04-29-comprehensive-tech-debt-cleanup/`) removed several patterns
that had previously grown back. Do not reintroduce any of them. The architectural-lint test
(`tests/ArchitectureInvariantsTests.cpp`, registered as `ArchitectureInvariants/SoftChecks` in the
default `ctest` run) hard-fails on most of these; the rest are policy and reviewer-enforced.

Hard-fail invariants (lint will reject the change):

- No `friend class` or `friend struct` in `src/workspace/*`. Use a service interface or an
  explicit narrow accessor instead.
- No `WorkspaceShell&` or `WorkspaceShell*` parameters in any
  `src/workspace/Workspace*Coordinator*.h` constructor. Inject the specific service interface
  (`EditorTabService`, `ProjectCatalogService`, `PromptSurfaceService`, `SidebarService`,
  `CompareMergeService`, `TerminalPanelService`, `PluginRuntimeService`, `PersistenceService`) or
  a small callback bundle.
- No `try`/`catch` around `std::stoi`, `std::stoll`, `std::stoull`, `std::stof`, or `std::stod`.
  Use the non-throwing `util/Parse.h` helpers (`ParseInt`, `ParseInt64`, `ParseSize`,
  `ParseFloat`) and handle `std::optional` directly.
- `src/workspace/shell/WorkspaceShell.h` ≤ 400 code lines, `src/workspace/shell/WorkspaceShell.cpp` ≤ 600 code lines, and `src/workspace/shell/WorkspaceShellMembers.inc` under its own ratcheting code-line cap.
  All three are hard-linted and count CODE lines only. The `.inc` belongs in that list: it is
  the class's real declaration surface (included inside the class body), so the header's 400
  bounds one of the class's two halves, not the class (TD-2026-09-13-293a). Its cap ratchets
  DOWN only — take the number from `tests/ArchitectureInvariantsTests.cpp`, not from memory.
  Add behavior to a service, not the shell.
- The render translation units covered by `CheckRenderSurfaceStateAccess`
  (`WorkspaceShellRenderFrame`, `WorkspaceShellRenderOverlay`, `WorkspaceShellRenderTextInput`,
  `WorkspaceShellRenderSidebar`, `WorkspaceShellRenderBottomPanel`, `WorkspaceShellHoverPopup`,
  `WorkspaceShellHoverTargets`, `DebugPaneRender`) consume view models built by `RenderViewModelBuilder`. Do not read
  `context_.current_project_state` or call `CurrentTextInputSurface(...)` from these files.
- Render view models are owned/precomputed data, not live state pointers
  (`CheckRenderViewModelsOwnProjectState`, TD-2026-07-17-084/26): `OverlayState` must not appear
  in `RenderViewModelBuilder.h` at all, and a `ProjectWorkspaceState` pointer is allowed only in
  the two documented escape hatches (`FrameSurfaceViewModel`, `SidebarSurfaceViewModel`) until
  their own view-model passes land. The converted render TUs (`WorkspaceShellRenderOverlay`,
  `WorkspaceShellRenderBottomPanel`, `WorkspaceShellRenderTextInput`, `WorkspaceShellHoverPopup`,
  `WorkspaceShellHoverTargets`, `DebugPaneRender`) must not name either broad state type.
- The two sides of a conflict, a merge or a compare are **Local** and **Remote** in every
  user-visible string: buttons, menus, palette titles and command ids, toasts, row details, pane
  captions, compare titles. Never mine / yours / ours / theirs / the host's, nor current /
  incoming (git's own mergetool calls them `$LOCAL` and `$REMOTE`). "Host" stays the name of the
  remote *machine* ("Open Folder on Host…", "Changed on Host"); prose that merely addresses the
  user ("your ~/.ssh/config") is not a side name. Enforced by `CheckSideNamesAreLocalAndRemote`
  (string literals naming a side after Keep/Take/Accept/Copy/Use, as a parenthesised suffix, or
  as a whole caption).

Policy invariants (no automated lint, but reviewers will reject):

- No project-level or shell-level fallback editor viewport. Resolve the active editor target
  through `EditorTabService::ActiveViewport()` (or equivalent typed accessor). The legacy symbols
  `text_viewport_` and `current_project_state_.text_viewport` were deleted intentionally; do not
  reintroduce equivalents under a new name. (The `text_viewport_` spelling itself is now
  hard-linted by `CheckNoFallbackEditorViewportSymbols`; renamed revivals stay reviewer-enforced.)
- No bespoke per-section parser for project state, user config, and session restore.
  These artifacts route through `PersistedRecordReader`/`PersistedRecordWriter`
  and `PersistenceService`. Add a typed record, do not hand-roll a text format.
- No new direct file I/O for workspace/session/config/conversation state outside
  `PersistenceService`. (Now hard-linted as a ratchet: `CheckPersistenceFileIoBoundary` bans
  raw file streams across `src/workspace/*` outside a documented three-TU allowlist —
  PersistenceService, ControlChannelService, LspService.)
- No per-surface duplicate of single-line edit operations (insert, backspace, delete-forward,
  caret movement, selection, copy, cut, paste, select-all). Single-line surfaces consume
  `editor/SingleLineEditor.{h,cpp}` plus `editor/SingleLineKeyHandler.{h,cpp}`.
- No `lua_State*` outside `plugin/LuaRuntime.{h,cpp}`. Plugin extension-surface modules consume
  the runtime through opaque handles.
- Plugin extension surfaces stay split across focused translation units (commands, sidebars,
  syntax, diagnostics, hover, providers, lifecycle); do not collapse them back into a monolithic
  `PluginHost.cpp`. Each `src/plugin/*.cpp` translation unit stays at or below 800 lines.
- View models do not hold pointers or references to `WorkspaceShell`, coordinators, or services.
  They are POD-like structs populated by the builder.
- Responsive shell surfaces (`LayoutModeService`, `StatusBarService`, `SettingsOverlayService`) stay
  host-owned and service-backed. Menu overflow, status-bar actions, Settings, and
  Help/About must route through action/service state and `RenderViewModelBuilder`, not plugin-owned
  rendering or render-TU product logic.
- Project reactivation paths do not reload plugins. Reactivation refresh uses
  `refresh_plugin_surfaces_for_reactivation` and must not call
  `ReloadPluginsForCurrentProject`. (Now hard-linted for the ProjectCatalogService flow by
  `CheckReactivationDoesNotReloadPlugins`, with a loud-missing-target vacuity guard.)
- Compare-surface rendering is structurally gated by `RenderViewModelBuilder` output. Render units
  must not inspect shell or project state to decide whether compare content renders.
- Per-frame preparation work executes once per frame. Per-clip entry points must not rebuild frame
  layout, normalize state, or rebuild render view models.
- Plugin reload/shutdown must drain async process workers before plugin teardown using the bounded
  drain seam; do not call teardown directly without the drain step.

The 2026-05-02 `deferred-work-and-throughput-pass` added four further invariants:

- No synchronous subprocess wait on the main thread in workspace code (`src/workspace/`). New code
  that needs subprocess results must dispatch through `ProjectBackgroundExecutor` or a similar
  background executor and deliver results via SDL user event. The
  `CheckNoSynchronousSubprocessWaitInWorkspace` lint rule hard-fails on direct `Subprocess::Wait`,
  `waitpid`, `WaitForSingleObject`, or `WaitForMultipleObjects` calls in workspace TUs.
- LSP `textDocument/didOpen` and `textDocument/didChange` notifications must not be sent
  synchronously on the `ActivateTab` call path. The hydration notification must be posted
  asynchronously so the tab is visible before any LSP acknowledgement is awaited.
- `ComputeLayout()` must be skipped when the layout-dirty flag is clear. Layout recomputation must
  only run when a window-resize, divider-drag, sidebar-toggle, or panel-toggle event has set the
  dirty flag; do not call it unconditionally from `PrepareFrameOnce`.
- The SDL event loop must never use a zero-delay `SDL_PollEvent` spin at idle. Use the
  `IdleHint`-driven strategy: `Full` hint → `SDL_PollEvent`; `CaretOnly` hint →
  `SDL_WaitEventTimeout(caret_remaining_ms)`; `Idle` hint → `SDL_WaitEvent`.

The 2026-05-06 `codebase-cleanup-perf-and-debt` adds four further hard invariants:

- No legacy persistence symbols (`WorkspacePersistenceLegacyFormat`, `EncodeSessionNodePath`,
  `DecodeSessionNodePath`, `ParseUserConfigText`, `ParseProjectConfigText`,
  `ParseProjectSessionText`, `ParseWorkspaceSessionText`) anywhere in `src/`, `tests/`, or
  `tools/`; this prevents accidental resurrection of the deleted importer path.
- No `platform::RunSubprocess(...)` calls in `src/workspace/*.cpp`; workspace subprocess work must
  route through `ProjectBackgroundExecutor` to keep shell-thread latency predictable.
- Render translation units (`WorkspaceShellRender*.cpp`) must not materialize new strings in hot
  paths; string assembly belongs in `RenderViewModelBuilder` so per-frame rendering stays lean.
  This includes the compare/merge surface render TUs `WorkspaceShellRenderCompare.cpp` and
  `WorkspaceShellRenderMerge.cpp` — the `WorkspaceShellRender*` prefix is what places them under the
  lint set, so do not rename them back to `*CompareRender`/`*MergeRender`. They reuse scratch
  `DecoratedTextRow` members instead of a fresh per-row row and truncate hot labels through
  `TruncateLabelView` (`CheckCompareMergeRenderUsesScratchRows`).
- A `Make*Service()` / `Make*Coordinator()` result must be bound by REFERENCE (`auto&`, or
  `Type&`), never copied. These factories return a reference into the shell-owned
  `ShellGlueCache`; a value binding copies the whole coordinator — two references and
  seventeen `std::function`s — on every call, which silently undoes the caching. Twenty-two
  sites had drifted back to the value spelling by 2026-09-29, one of them inside the
  watcher's per-changed-file loop. Enforced by `CheckShellGlueIsBoundByReference`, which is
  loud rather than vacuous when it finds no factory calls at all.
- Capturing one of those locals BY VALUE into a lambda is the same copy per hook, and a
  reference binding does not make it free — `[svc]` on a reference copies the referent.
  Enforced by `CheckFactoryResultsAreNotCapturedByValue` (TD-2026-08-10-177), which matches
  BOTH the value and reference spellings: it once matched only `auto x = Make…()`, so the
  sweep to `auto&` left it scanning nothing and only its missing-target guard said so.
- Write and start APIs whose dropped result is a silent failure are `[[nodiscard]]`:
  `util::WriteTextFileAtomically`, `TextViewport::Save`, `FileWriteGate::WriteText`,
  `ControlSocketServer::Start`, `ControlChannelService::Start`. Deliberate discards use
  `(void)` WITH the reason — an ignored `Start()` was once a control channel that did not
  exist with nothing anywhere explaining why. Add the attribute to new APIs of this shape.
- `TextViewport` non-const mutation paths must not copy `document_->lines` wholesale; undo and
  edit flows should capture only the affected ranges to avoid large-buffer copy regressions.

The durable contracts these rules implement live in
`openspec/specs/workspace-architecture/spec.md`, `openspec/specs/persisted-state-format/spec.md`,
and `openspec/specs/shared-edit-primitives/spec.md`. Update those specs in the same change when a
durable invariant moves.
