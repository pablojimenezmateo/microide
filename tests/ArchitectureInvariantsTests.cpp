#include "TestSupport.h"

#include "architecture/ArchitectureFileScanner.h"
#include "architecture/ArchitectureRuleHelpers.h"
#include "architecture/PluginArchitectureRules.h"
#include "architecture/TerminalArchitectureRules.h"
#include "architecture/WorkspaceArchitectureRules.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace microide::tests {
namespace {

// Shared assertion: report every rule result and fail if any hard rule was
// violated. Soft (non-hard_fail) rules still print but do not fail the test.
void AssertRuleResultsPass(const std::vector<architecture::RuleResult>& results) {
  bool hard_failure = false;
  for (const architecture::RuleResult& result : results) {
    architecture::ReportRule(result);
    if (result.hard_fail && !result.violations.empty()) {
      hard_failure = true;
    }
  }
  Expect(!hard_failure, "hard-fail architecture invariants should have no violations");
}

// One workspace rule, run in isolation. Registered once per rule (see
// RegisterArchitectureInvariantsTests) so ctest sharding runs the individually
// slow, std::regex-heavy rules in parallel instead of as one ~30s serial test.
void RunWorkspaceRuleTest(architecture::ArchitectureRuleFn fn) {
  AssertRuleResultsPass({fn(architecture::RepoRoot())});
}

void TestArchitecturePluginRules() {
  AssertRuleResultsPass(architecture::RunPluginArchitectureRules(architecture::RepoRoot()));
}

void TestArchitectureTerminalRules() {
  AssertRuleResultsPass(architecture::RunTerminalArchitectureRules(architecture::RepoRoot()));
}

void TestArchitectureFileSizes() {
  const std::filesystem::path repo_root = architecture::RepoRoot();
  std::vector<architecture::RuleResult> results;
  const auto run_rule = [&](const char* label, auto&& fn) {
    try {
      results.push_back(fn(repo_root));
    } catch (const std::exception& error) {
      throw std::runtime_error(std::string(label) + ": " + error.what());
    }
  };

  run_rule("CheckShellFileSize(WorkspaceShell.h)", [&](const std::filesystem::path& root) {
    return architecture::CheckShellFileSize(root, "src/workspace/WorkspaceShell.h", 400);
  });
  run_rule("CheckShellFileSize(WorkspaceShell.cpp)", [&](const std::filesystem::path& root) {
    return architecture::CheckShellFileSize(root, "src/workspace/WorkspaceShell.cpp", 600);
  });
  run_rule("CheckShellFileSize(WorkspaceShellTestAccess.h)",
           [&](const std::filesystem::path& root) {
             return architecture::CheckShellFileSize(root, "src/workspace/WorkspaceShellTestAccess.h",
                                                     600);
           });
  run_rule("CheckShellFileSize(WorkspaceShellMembers.inc)",
           [&](const std::filesystem::path& root) {
             // 1559: +1 for the Phase B CodeLensCommandAtPosition entry point. It
             // must be a member (it reaches private shell state and is called from
             // the mouse TU); its per-viewport resolver stays a file-local free
             // function so the shell surface grows by exactly one declaration.
             // 1560: +1 for the Phase C ToggleSelectedPluginSidebarItem entry point,
             // which routes the tree-sidebar twisty toggle from the mouse/key TUs
             // into the SidebarService (parallel to OpenSelectedPluginSidebarItem).
             // 1571: +11 for the Phase C follow-up signature-help popup: the
             // SignatureHelpPopup state struct (7), its optional shell member (1),
             // and the ShowSignatureHelpPopup / MaybeExpireSignatureHelp /
             // RenderSignatureHelpPopup entry points (3). The popup is shell-owned
             // (parallel to the hover popup) so it stays off the project-state path.
             // 1572: +1 for the Phase C follow-up ShowOutlineSidebar entry point,
             // the document-outline sidebar's host-side show wrapper (parallel to
             // ShowTestsSidebar).
             // 1575: +3 for Phase D presentation registries: the two host-owned
             // registry members (WorkspaceThemeRegistry, WorkspaceFileIconRegistry)
             // and the RebuildPresentationRegistries entry point that repopulates
             // them on plugin reload (parallel to RebuildPhase4Registries).
             // 1581: +6 for Phase E plugin surfaces: the host-owned
             // SurfaceTextureCache member and the ActivatePluginSurfacePreview /
             // SyncPluginSurfacePreviewClosed / RenderPluginSurfaceInto entry
             // points that drive the preview panel + inline insets.
             // 1584: +3 for the Phase E1 DrawEditorInsets entry point that paints
             // inline-surface insets into the editor's inert row-gaps.
             // 1586: +2 for the Phase E2 AboveLensCommandAtPosition entry point
             // that resolves above-line code-lens inset clicks (gap-aware) to the
             // bound command, paired with the existing EOL CodeLensCommandAtPosition.
             // 1587: +1 for ApplyPluginWorkspaceEdit, the host-owned entry point that
             // applies plugin ctx.editor.apply_edits requests through the real
             // viewport edit/undo primitives (SEAM 2 of VSCode-portability work).
             // 1591: +4 for the SEAM 1 reactive-editor-event seam: the
             // PluginEditorEventTracker member plus the RefreshPluginEditorEventInterest
             // / SamplePluginEditorEvents / DispatchDuePluginEditorEvents entry points
             // that debounce on_buffer_change/cursor/selection dispatch.
             // 1597: +6 for the ghost-text (Copilot inline suggestion) seam: the
             // PublishPluginGhostText / ClearPluginGhostText entry points (plugin
             // publish + clear), plus AcceptGhostText / DismissGhostText /
             // InvalidateGhostTextIfStale that handle Tab-accept, Esc-dismiss, and
             // synchronous staleness invalidation. State itself lives in the lazy
             // PluginEditorPresentation bundle, so the shell surface grows only by
             // these declarations.
             // 1598: +1 for plugin_thread_event_type_, the SDL wake event for the
             // dedicated plugin worker thread that runs Lua off the UI thread.
             // The thread/queues live in plugin/, so the shell surface grows only by
             // this one wake-event handle.
             // 1596: -2 after deleting the dead plugin subprocess-callback
             // subsystem (its shell consume-callback decl + SDL wake-event handle).
             // 1600: +4 for NotifyPluginCommandOutcome, the shared toast helper that
             // surfaces an async plugin command's outcome (run on the worker,
             // delivered on the drain) for both the keybinding and command-prompt
             // entry points instead of duplicating the logic at each.
             // 1616: +16 for the PluginHoverCache state machine + kickoff helper that
             // moves plugin hover off the synchronous hit-test path onto the worker
             // (sibling of the existing EditorHoverTarget / PendingHoverEval state).
             // 1617: +1 for ConsumeReloadResult, the post-reload consumption helper that
             // runs registry rebuilds + sidebar/syntax refresh from the non-blocking
             // ReloadAsync completion instead of inline after a UI-parking reload.
             // 1623: +6 for two deferred-settings features finishing the settings overhaul:
             // the "after delay" autosave debounce (edit epoch + revision + armed state,
             // MaybeArmAutosaveTimer / NextAutosaveDelayMs) and the terminal's own font
             // (terminal_text_renderer_ + last-applied size, PanelTextRenderer /
             // ApplyTerminalFontPreferences) so terminal.font_size/font_family are live.
             // 1626: +3 for the settings-review bug-fix batch: ApplyCanonicalPreferenceSideEffects
             // (shared canonical materialize + live colorscheme apply on write/reset),
             // LineNumbersEnabled (gutter-width parity across render/hit-test when
             // editor.line_numbers is off), and autosave_suppress_format_on_save_ (skip the
             // synchronous formatter during autosave so focus-loss/debounce never blocks the UI).
             // 1637: +11 for the font-selection overhaul: the native "Choose font file…" picker
             // (OpenNativeFontFilePicker / OnFontFileDialogComplete / ConsumePendingFontFileDialogResult
             // + FontFileDialogState), the installed-font combobox (MoveSettingsFontPicker /
             // ApplySettingsFontPickerIndex / CachedFontFamilies + the lazily cached family list),
             // and last_applied_settings_revision_ (gates ApplyLiveSettings to a single compare
             // when no setting changed).
             // 1639: +2 for the settings-review bug-fix batch: autosave_last_viewport_ (tell a
             // genuine edit apart from a tab switch so a pending after-delay autosave survives the
             // switch) and last_applied_terminal_font_settings_revision_ (gate the per-frame terminal
             // font apply on the settings revision, keeping the steady-state path allocation-free).
             // 1640: +1 for ProjectTabStripVisible (single-sources the "hide project tab strip when
             // a single project is open" predicate across the 4 ComputeLayout call sites so render
             // and hit-test agree on the strip's collapsed geometry).
             // 1642: +2 for caching that predicate's settings lookup: project_tabs_hide_when_single_
             // + its revision stamp memoize the string-keyed GetSettingValue (and its default-value
             // allocation) so ProjectTabStripVisible re-resolves only on a settings-store revision
             // bump, keeping the per-mouse-move window-drag hit-test's uncached ComputeLayout off the
             // string-lookup path.
             // 1644: +2 for the resilience-fix members: last_applied_follow_out_of_root_symlinks_
             // (change-detect the project.follow_out_of_root_symlinks toggle so ApplyLiveSettings
             // re-scans only when it flips) and replace_all_aggregate_cap_bytes_ (a test seam for the
             // Replace-All buffer ceiling).
             // 1645: +1 for last_applied_files_exclude_ (change-detect the project.files_exclude
             // edit so ApplyLiveSettings re-applies exclude globs + re-scans only on an actual change).
             // 1646: +1 for NotifyLspBufferOpen (engages the LSP on file open / tab activation +
             // session restore, not only on the first edit or go-to-definition).
             // 1653: +7 for the crash-safety session-flush debounce (MaybeArmSessionFlushTimer /
             // NextSessionFlushDelayMs / FlushSessionStateForCrashSafety + its four debounce fields),
             // an always-on net that stages unsaved buffer content to the durable session store so a
             // crash / kill -9 loses at most the debounce window instead of everything since the last
             // event-driven save.
             // 1658: +5 for the LSP feature-wiring members: ComputeCenteredMenuOverlayRect (the
             // centered code-action menu), QueryLspDocumentSymbolsForOutline (LSP documentSymbol
             // outline fallback, 3 lines), and KickOffLspHover (LSP hover fallback).
             // 1668: +10 for project-wide rename: ApplyRenameWorkspaceEdit / CommitPendingRenameSave
             // / DiscardPendingRenameSave + the PendingRenameSave struct and its optional member
             // (open + apply + save the files a rename touches that were not already open).
             // 1671: +3 for all-groups dirty flush (VSCode "Save All"): SaveGroupTab wrapper +
             // DirtyEditorGroupTabs / DirtyEditorGroupTabsForProject accessors, so autosave and
             // save-on-quit flush a buffer dirtied in the non-focused split group too.
             // 1672: +1 for RebaseActiveTerminalForScrollbackTrim (rebases the terminal's
             // absolute-row mirrors when scrollback is trimmed, so a scrolled-up view / held
             // selection tracks the same content instead of jumping forward by the trim batch).
             // 1690: +18 for the async compare/ref picker: mailbox + monotonic generation +
             // three injectable git providers, plus Request*/Apply*/ComparePickerRequestCurrent
             // so the blocking git history/branch queries run off the render thread.
             // 1691: +1 for interactive_background_executor_, a dedicated lane so the
             // compare/ref picker git queries never queue behind an in-flight sidebar git status.
             // 1692: +1 for media_background_executor_ (TD-2026-07-17-044), a dedicated lane so
             // plugin raster decode never queues ahead of core git/diff/merge/project-state work.
             // 1693: net +1 for TD-2026-07-17A-004 folding-refresh hoist: replaced the render-only
             // EnsureGroupFoldingModelFresh with RefreshEditorFoldingModels (once-per-frame prep)
             // + the non-mutating GroupFoldingModelPtr the render TU reads.
             // 1697: +4 for TD-2026-07-17-081/082 off-thread forced rescan:
             // file_index_refresh_mailbox_ + RequestFileIndexRefresh()/ApplyForcedFileIndexRefresh()
             // decls, so the whole-tree scan + per-file stat runs off the shell thread on manual
             // refresh / exclude edits and applies via the mailbox.
             // 1700: +3 for TD-2026-07-17-021 off-thread project replace-all:
             // project_replace_mailbox_ + project_replace_generation_ + ApplyProjectReplaceOutcome()
             // decl, so the bulk read/replace/atomic-write runs off the shell thread.
             // 1702: +2 for TD-2026-07-17-011 WorkspaceEdit resource ops: the
             // ApplyRenameWorkspaceEdit resource_ops parameter + the PendingRenameSave
             // resource_ops field (the confirm prompt must stash the file ops with the
             // edits). The apply logic itself lives in LspService, not the shell.
             // 1699: net -3 for TD-2026-07-17-084/083: the SingleLineViewMetrics struct
             // moved to workspace/SingleLineViewMetrics.h (-4, replaced by a using alias)
             // and the frame-prep PrepareCommitBodyViewportForFrame entry point (+1)
             // that sizes/clamps the commit-draft body viewport before paint.
             return architecture::CheckShellFileSize(root, "src/workspace/WorkspaceShellMembers.inc",
                                                     1699);
           });

  AssertRuleResultsPass(results);
}

void TestTryCatchStoScanner() {
  const std::string allowed = R"cpp(
// try { std::stoi("1"); } catch (...) {}
const char* s = "try { std::stod(x); } catch (...) {}";
try {
  const auto value = util::ParseInt("42");
  (void)value;
} catch (const std::exception&) {
}
)cpp";
  const std::string flagged = R"cpp(
try
{
  int x = std::stoi("7");
  try { (void)std::stoll("9"); } catch (...) {}
}
catch (const std::exception&) {
}
)cpp";
  Expect(architecture::FindTryCatchStoViolations(allowed).empty(),
         "scanner should ignore comments/strings and non-sto try blocks");
  Expect(architecture::FindTryCatchStoViolations(flagged).size() == 2,
         "scanner should catch nested/multiline try std::sto usage");
}

void TestCountCodeLinesScanner() {
  const std::string snippet =
      "int x = 1;            // counted: code + trailing comment\n"
      "                      // not counted: comment only\n"
      "\n"  // not counted: blank
      "   \t  \n"  // not counted: whitespace only
      "/* block-only line */\n"  // not counted: comment only
      "const char* s = \"// not a comment\";\n"  // counted: code around a string
      "}  // counted: brace with trailing comment\n"
      "int y = 2;";  // counted: final line without trailing newline
  Expect(architecture::CountCodeLines(snippet) == 4,
         "CountCodeLines should count only lines holding real code, excluding "
         "comment-only and blank lines");
  Expect(architecture::CountCodeLines("") == 0, "empty input has no code lines");
  Expect(architecture::CountCodeLines("// only a comment\n") == 0,
         "a lone comment line counts as zero code lines");
}

void TestArchitectureInvariantTargetedScannerFixtures() {
  TemporaryDirectory temp_dir;
  const auto& root = temp_dir.path();
  std::filesystem::create_directories(root / "src/workspace");
  std::filesystem::create_directories(root / "src/editor");
  std::filesystem::create_directories(root / "tests");
  std::filesystem::create_directories(root / "tools");

  WriteFile(root / "src/workspace/NeedsExecutor.cpp",
            "void F(){ platform::RunSubprocess({\"echo\"}, {}); }\n");
  WriteFile(root / "src/workspace/WorkspaceShellRenderSidebar.cpp",
            "std::string F(){ return std::string(\"search> \") + std::string(\"x\"); }\n");
  WriteFile(root / "src/workspace/WorkspaceLanguageContract.cpp", "// lang contract fixture\n");
  WriteFile(root / "src/editor/IndentGuides.cpp", "// indent guides fixture\n");
  WriteFile(root / "src/editor/SnippetEngine.cpp", "// snippet engine fixture\n");
  WriteFile(root / "src/editor/FoldingModel.cpp", "void leak(lua_State* L){ (void)L; }\n");
  // Violating fixture for the TextViewport no-whole-buffer-materialization rules.
  // Written to TextViewportEditEngine.cpp — the file the rules actually scan (a
  // prior fixture wrote TextViewport.cpp, so the expectations passed vacuously via
  // the could-not-locate-body violation without exercising the pattern scan).
  // Covers all three pattern families: full-vector assignment, ToVector()/
  // Snapshot() materialization, and begin()/end() snapshot-backed iteration.
  WriteFile(root / "src/editor/TextViewportEditEngine.cpp",
            "std::size_t TextViewport::ReplaceAll(std::string_view, std::string_view) {\n"
            "  std::vector<std::string> before = document_->lines;\n"
            "  (void)before;\n"
            "  return 0;\n"
            "}\n"
            "std::optional<std::size_t> TextViewport::ReplaceAllRanges(int) {\n"
            "  auto all = document_->lines.ToVector();\n"
            "  return all.size();\n"
            "}\n"
            "bool TextViewport::ApplyLineEdit(std::size_t, std::size_t,\n"
            "                                 const std::vector<std::string>&) {\n"
            "  auto snap = document_->lines;\n"
            "  return !snap.empty();\n"
            "}\n"
            "bool TextViewport::ApplyRangeEdit(int) {\n"
            "  for (auto it = document_->lines.begin(); it != document_->lines.end(); ++it) {}\n"
            "  return true;\n"
            "}\n"
            "void TextViewport::ApplyHistoryEntry(int) {\n"
            "  (void)document_->lines.Snapshot();\n"
            "}\n");
  WriteFile(root / "tests/LegacySymbolFixture.cpp", "void X(){ WorkspacePersistenceLegacyFormat x; }\n");

  Expect(!architecture::CheckEssentialEditorCppModulesDoNotTouchLuaState(root).violations.empty(),
         "lua_State pointers should not appear in FoldingModel.cpp fixture");

  Expect(!architecture::CheckNoLegacyPersistenceSymbols(root).violations.empty(),
         "legacy-persistence rule should catch legacy symbols");
  Expect(!architecture::CheckNoSynchronousSubprocessInWorkspace(root).violations.empty(),
         "workspace subprocess rule should catch synchronous subprocess calls");

  WriteFile(root / "src/workspace/SomeCoordinator.cpp",
            "void F(State& s){ s.overlay.visible = false; }\n");
  Expect(!architecture::CheckOverlayDismissalIsCentralized(root).violations.empty(),
         "overlay-dismissal rule should catch a bare overlay.visible = false");
  WriteFile(root / "src/workspace/WorkspaceShellOverlay.cpp",
            "void F(State& s){ s.overlay.visible = false; }\n");
  WriteFile(root / "src/workspace/SomeCoordinator.cpp",
            "void F(State& s){ HideOverlay(s); }\n");
  Expect(architecture::CheckOverlayDismissalIsCentralized(root).violations.empty(),
         "overlay-dismissal rule should accept the canonical file and HideOverlay rewrite");

  WriteFile(root / "src/plugin/SomeInterop.cpp",
            "int F(lua_State* s, const std::string& e){ return luaL_error(s, \"%s\", e.c_str()); }\n");
  Expect(!architecture::CheckPluginLuaErrorDoesNotLongjmpOverCppLocals(root).violations.empty(),
         "plugin Lua-error rule should catch a luaL_error that longjmps over a std::string");
  WriteFile(root / "src/plugin/SomeInterop.cpp",
            "int F(lua_State* s, const std::string& e){\n"
            "  lua_error_util::PushMessage(s, e, \"fallback\");\n"
            "  return lua_error(s);\n"
            "}\n");
  Expect(architecture::CheckPluginLuaErrorDoesNotLongjmpOverCppLocals(root).violations.empty(),
         "plugin Lua-error rule should accept the PushMessage + lua_error rewrite");

  WriteFile(root / "src/plugin/FieldReader.cpp",
            "void F(lua_State* s){ lua_getfield(s, 1, \"label\"); lua_pop(s, 1); }\n");
  Expect(!architecture::CheckPluginFieldReadsAreMetamethodProtected(root).violations.empty(),
         "field-read rule should catch a raw lua_getfield in a plugin TU");
  WriteFile(root / "src/plugin/FieldReader.cpp",
            "void F(lua_State* s){ lua_interop::GetFieldProtected(s, 1, \"label\"); "
            "lua_pop(s, 1); }\n");
  Expect(architecture::CheckPluginFieldReadsAreMetamethodProtected(root).violations.empty(),
         "field-read rule should accept the GetFieldProtected rewrite (and raw lua_rawgeti)");
  // The sanctioned definition site is exempt: PluginLuaInterop.cpp may use the raw
  // lua_getfield/lua_gettable that GetFieldProtected is built from.
  WriteFile(root / "src/plugin/PluginLuaInterop.cpp",
            "void G(lua_State* s){ lua_getfield(s, 1, \"x\"); lua_gettable(s, 1); }\n");
  Expect(architecture::CheckPluginFieldReadsAreMetamethodProtected(root).violations.empty(),
         "field-read rule should exempt PluginLuaInterop.cpp (home of GetFieldProtected)");

  Expect(!architecture::CheckRenderTuDoesNotMaterializeStrings(root).violations.empty(),
         "render materialization rule should catch string construction in render TU");
  Expect(!architecture::CheckTextViewportNoFullDocCopy(root).violations.empty(),
         "batch-replace rule should catch the full-vector copy and ToVector() materialization");
  Expect(
      !architecture::CheckTextViewportApplyPipelineNoFullDocumentLineSnapshot(root).violations.empty(),
      "apply-pipeline rule should catch snapshot copy, begin()/end() iteration, and Snapshot()");

  // Positive control: a clean fixture using only the bounded accessors must pass
  // BOTH rules with zero violations. This is the vacuity guard — a missing file
  // or unlocatable signature yields could-not-locate violations and fails here.
  WriteFile(root / "src/editor/TextViewportEditEngine.cpp",
            "std::size_t TextViewport::ReplaceAll(std::string_view, std::string_view) {\n"
            "  return document_->lines.size();\n"
            "}\n"
            "std::optional<std::size_t> TextViewport::ReplaceAllRanges(int) {\n"
            "  return document_->lines.LineView(0).size();\n"
            "}\n"
            "bool TextViewport::ApplyLineEdit(std::size_t, std::size_t,\n"
            "                                 const std::vector<std::string>&) {\n"
            "  return !document_->lines.empty();\n"
            "}\n"
            "bool TextViewport::ApplyRangeEdit(int) {\n"
            "  document_->lines.ReplaceLineRange(0, 0, {});\n"
            "  return true;\n"
            "}\n"
            "void TextViewport::ApplyHistoryEntry(int) {\n"
            "  (void)document_->lines.SliceLines(0, 1);\n"
            "}\n");
  Expect(architecture::CheckTextViewportNoFullDocCopy(root).violations.empty(),
         "batch-replace rule should accept bounded size/LineView accessors");
  Expect(architecture::CheckTextViewportApplyPipelineNoFullDocumentLineSnapshot(root)
             .violations.empty(),
         "apply-pipeline rule should accept bounded SliceLines/ReplaceLineRange edits");

  WriteFile(root / "src/workspace/RenderViewModelBuilder.cpp",
            "int Parse() { return static_cast<int>(std::stol(\"3\")); }\n");
  Expect(!architecture::CheckNoStdStoInRenderOrBuilderTus(root).violations.empty(),
         "std::sto* rule should catch std::stol in RenderViewModelBuilder.cpp");
  WriteFile(root / "src/workspace/RenderViewModelBuilder.cpp",
            "int Parse() { return util::ParseInt(\"3\").value_or(3); }\n");
  Expect(architecture::CheckNoStdStoInRenderOrBuilderTus(root).violations.empty(),
         "std::sto* rule should accept the util::ParseInt rewrite");

  // Status-bar async rule: re-anchored to the TUs that actually host the
  // frame-path refresh (the retired WorkspaceShellChrome.cpp target made it
  // silently vacuous), with the missing-target case failing loudly.
  WriteFile(root / "src/workspace/WorkspaceShellPresentation.cpp",
            "std::string F(const Repo& repo) { return repo.Execute({\"symbolic-ref\"}).output; }\n");
  WriteFile(root / "src/workspace/StatusBarModelService.cpp", "// clean model-build fixture\n");
  Expect(!architecture::CheckStatusBarRefreshIsAsyncOnly(root).violations.empty(),
         "status-bar rule should catch synchronous repo.Execute in the frame-path TU");
  WriteFile(root / "src/workspace/WorkspaceShellPresentation.cpp",
            "std::string F() { return std::string(\"async-only\"); }\n");
  Expect(architecture::CheckStatusBarRefreshIsAsyncOnly(root).violations.empty(),
         "status-bar rule should pass on async-only fixtures for both target TUs");
  std::filesystem::remove(root / "src/workspace/StatusBarModelService.cpp");
  Expect(!architecture::CheckStatusBarRefreshIsAsyncOnly(root).violations.empty(),
         "a moved/renamed status-bar rule target must fail loudly, not pass vacuously");
  WriteFile(root / "src/workspace/StatusBarModelService.cpp", "// clean model-build fixture\n");

  WriteFile(root / "src/workspace/RenderViewModelBuilder.h",
            "struct SidebarSurfaceViewModel { std::string query_fallback_text; "
            "std::string replace_fallback_text; };\n");
  Expect(!architecture::CheckSidebarSurfaceFallbackUsesStringView(root).violations.empty(),
         "sidebar-fallback rule should catch std::string fallback fields");
  WriteFile(root / "src/workspace/RenderViewModelBuilder.h",
            "struct SidebarSurfaceViewModel { std::string_view query_fallback_text; "
            "std::string_view replace_fallback_text; };\n");
  Expect(architecture::CheckSidebarSurfaceFallbackUsesStringView(root).violations.empty(),
         "sidebar-fallback rule should pass on the string_view fixture");

  WriteFile(root / "src/workspace/WorkspaceShellRenderSidebar.cpp",
            "void F(char m, const std::string& s){ auto x = std::string(1, m); "
            "auto y = std::string(\"foo: \") + s; (void)x; (void)y; }\n");
  Expect(!architecture::CheckRenderTuDoesNotMaterializeSingleCharOrPrefixStrings(root).violations.empty(),
         "render-string rule should catch single-char std::string and literal+ident concat");
  WriteFile(root / "src/workspace/WorkspaceShellRenderSidebar.cpp",
            "void F(char m, const std::string& s){ std::string_view x(&m, 1); (void)x; (void)s; }\n");
  Expect(architecture::CheckRenderTuDoesNotMaterializeSingleCharOrPrefixStrings(root).violations.empty(),
         "render-string rule should pass on the string_view rewrite fixture");

  // The bare `"search> " + ident` form (no std::string wrapper) anchors on a
  // string-literal quote. BuildCodeMask flags that quote as non-code, so the
  // all-in-code predicate could never match it -- this rule was silently dead
  // until the trailing-anchored helper landed. Guard the live behavior here.
  WriteFile(root / "src/workspace/WorkspaceShellRenderSidebar.cpp",
            "void F(const std::string& q){ auto s = \"search> \" + q; (void)s; }\n");
  Expect(!architecture::CheckRenderTuDoesNotMaterializeStrings(root).violations.empty(),
         "render fallback rule should catch the bare \"search> \" + ident concat form");
  WriteFile(root / "src/workspace/WorkspaceShellRenderSidebar.cpp",
            "void F(std::string_view fallback){ DrawText(fallback); }\n");
  Expect(architecture::CheckRenderTuDoesNotMaterializeStrings(root).violations.empty(),
         "render fallback rule should pass when the fallback uses a precomputed view");

  // literal+ident concat in a hot editor render TU must fire (trailing-anchored).
  WriteFile(root / "src/editor/EditorViewRenderer.cpp",
            "void F(const std::string& s){ auto row = \"ln \" + s; (void)row; }\n");
  Expect(!architecture::CheckRenderTuDoesNotMaterializeSingleCharOrPrefixStrings(root).violations.empty(),
         "render-string rule should catch literal+ident concat in a hot editor render TU");
  WriteFile(root / "src/editor/EditorViewRenderer.cpp",
            "void F(std::string_view s){ DrawRow(s); }\n");
  Expect(architecture::CheckRenderTuDoesNotMaterializeSingleCharOrPrefixStrings(root).violations.empty(),
         "render-string rule should pass when the hot editor render TU avoids concat");

  WriteFile(root / "src/editor/EditorViewModel.h",
            "struct EditorViewModel { std::vector<std::size_t> sticky_lines; "
            "std::vector<OccurrenceRange> occurrence_ranges; };\n");
  Expect(!architecture::CheckEditorViewModelStickyAndOccurrenceAreSpans(root).violations.empty(),
         "editor view-model rule should catch owning vector fields");
  WriteFile(root / "src/editor/EditorViewModel.h",
            "struct EditorViewModel { std::span<const std::size_t> sticky_lines; "
            "std::span<const OccurrenceRange> occurrence_ranges; };\n");
  Expect(architecture::CheckEditorViewModelStickyAndOccurrenceAreSpans(root).violations.empty(),
         "editor view-model rule should pass on the span fixture");

  // Render view-model state-pointer ratchet (TD-2026-07-17-084/26): OverlayState is
  // banned outright in the view-model header, ProjectWorkspaceState is allowed only
  // inside the two documented escape-hatch structs, and the converted render TUs
  // must not name either broad state type.
  WriteFile(root / "src/workspace/RenderViewModelBuilder.h",
            "struct OverlaySurfaceViewModel { const OverlayState* state = nullptr; "
            "ProjectWorkspaceState* project_state = nullptr; };\n");
  Expect(architecture::CheckRenderViewModelsOwnProjectState(root).violations.size() == 2,
         "view-model state rule should catch both the OverlayState pointer and the "
         "non-allowlisted ProjectWorkspaceState pointer");
  WriteFile(root / "src/workspace/WorkspaceShellRenderOverlay.cpp",
            "void F(const ProjectWorkspaceState& s){ (void)s; }\n");
  WriteFile(root / "src/workspace/DebugPaneRender.cpp",
            "void G(const OverlayState& s){ (void)s; }\n");
  Expect(architecture::CheckRenderViewModelsOwnProjectState(root).violations.size() == 4,
         "view-model state rule should catch broad state type names in converted render TUs");
  // Positive control: the allowlisted structs may carry the pointer, everything
  // else owned — zero violations (kills the vacuous-pass mode).
  WriteFile(root / "src/workspace/RenderViewModelBuilder.h",
            "struct FrameSurfaceViewModel { ProjectWorkspaceState* project_state = nullptr; };\n"
            "struct SidebarSurfaceViewModel { std::string_view query_fallback_text; "
            "std::string_view replace_fallback_text; "
            "ProjectWorkspaceState* project_state = nullptr; };\n"
            "struct OverlaySurfaceViewModel { std::string label_storage; };\n");
  WriteFile(root / "src/workspace/WorkspaceShellRenderOverlay.cpp",
            "void F(const OverlaySurfaceViewModel& vm){ (void)vm; }\n");
  WriteFile(root / "src/workspace/DebugPaneRender.cpp",
            "void G(const DebugPaneSurfaceViewModel& vm){ (void)vm; }\n");
  Expect(architecture::CheckRenderViewModelsOwnProjectState(root).violations.empty(),
         "view-model state rule should pass on the owned-model + allowlisted-structs fixture");

  // Persistence file-I/O ratchet (TD-2026-07-17-032): raw streams in a workspace
  // TU outside the documented allowlist fail; the sanctioned service TU and
  // stream-free code pass (positive control against a vacuous rewrite).
  WriteFile(root / "src/workspace/SomeStateSaver.cpp",
            "void Save(const std::filesystem::path& p){ std::ofstream out(p); out << 1; }\n");
  Expect(!architecture::CheckPersistenceFileIoBoundary(root).violations.empty(),
         "persistence I/O rule should catch a raw ofstream in a non-allowlisted workspace TU");
  WriteFile(root / "src/workspace/SomeStateSaver.cpp",
            "void Save(persistence::PersistedRecordWriter& writer){ writer.Commit(); }\n");
  WriteFile(root / "src/workspace/PersistenceService.cpp",
            "void G(const std::filesystem::path& p){ std::ifstream in(p); (void)in; }\n");
  Expect(architecture::CheckPersistenceFileIoBoundary(root).violations.empty(),
         "persistence I/O rule should accept the record-writer rewrite and the sanctioned "
         "PersistenceService TU");

  // Reactivation must refresh, not reload, plugins (TD-2026-07-17-037).
  WriteFile(root / "src/workspace/ProjectCatalogService.cpp",
            "void Reactivate(){ ReloadPluginsForCurrentProject(); }\n");
  Expect(!architecture::CheckReactivationDoesNotReloadPlugins(root).violations.empty(),
         "reactivation rule should catch a plugin reload in the reactivation TU");
  WriteFile(root / "src/workspace/ProjectCatalogService.cpp",
            "void Reactivate(){ operations_.refresh_plugin_surfaces_for_reactivation(); }\n");
  Expect(architecture::CheckReactivationDoesNotReloadPlugins(root).violations.empty(),
         "reactivation rule should accept the refresh-seam rewrite");
  std::filesystem::remove(root / "src/workspace/ProjectCatalogService.cpp");
  Expect(!architecture::CheckReactivationDoesNotReloadPlugins(root).violations.empty(),
         "a moved reactivation TU must fail loudly, not pass vacuously");
  WriteFile(root / "src/workspace/ProjectCatalogService.cpp",
            "void Reactivate(){ operations_.refresh_plugin_surfaces_for_reactivation(); }\n");

  // Fallback editor-viewport symbol ban (TD-2026-07-17-037).
  WriteFile(root / "src/workspace/SomeShellState.h",
            "struct S { editor::TextViewport* text_viewport_ = nullptr; };\n");
  Expect(!architecture::CheckNoFallbackEditorViewportSymbols(root).violations.empty(),
         "viewport-symbol rule should catch a revived fallback viewport member");
  WriteFile(root / "src/workspace/SomeShellState.h",
            "struct S { };  // active viewport resolves through EditorTabService\n");
  Expect(architecture::CheckNoFallbackEditorViewportSymbols(root).violations.empty(),
         "viewport-symbol rule should pass once the member is gone");

  // Lua-behind-plugin-boundary rule (TD-2026-07-16-22). Fresh root: the shared
  // fixture tree above plants lua_State fixtures for other rules, which this
  // rule scans globally.
  {
    TemporaryDirectory lua_dir;
    const auto& lroot = lua_dir.path();
    std::filesystem::create_directories(lroot / "src/plugin");
    std::filesystem::create_directories(lroot / "src/workspace");
    std::filesystem::create_directories(lroot / "src/editor");
    // Direct leak: a workspace TU naming lua_State / including a Lua header.
    WriteFile(lroot / "src/workspace/BadDirect.cpp",
              "#include <lua.hpp>\nvoid F(lua_State* s){ (void)s; }\n");
    // Transitive leak: workspace includes a plugin facade that includes a
    // Lua-exposing plugin header.
    WriteFile(lroot / "src/plugin/Exposed.h", "#include <lua.hpp>\n");
    WriteFile(lroot / "src/plugin/CleanFacade.h", "#include \"plugin/Exposed.h\"\n");
    WriteFile(lroot / "src/workspace/BadTransitive.cpp",
              "#include \"plugin/CleanFacade.h\"\nvoid G(){}\n");
    const auto violating = architecture::CheckLuaStaysBehindPluginBoundary(lroot);
    Expect(violating.violations.size() == 3,
           "lua-boundary rule should catch the lua_State token, the Lua include, and the "
           "transitively Lua-exposing plugin include");

    // Positive control: comment-only lua_State mentions in a plugin header, a
    // Lua-free plugin include chain, and the sanctioned SyntaxDefinitionLoader
    // sandbox must produce ZERO violations (vacuity guard).
    WriteFile(lroot / "src/workspace/BadDirect.cpp",
              "#include \"plugin/PluginHost.h\"\nvoid F(PluginHost& h){ (void)h; }\n");
    WriteFile(lroot / "src/workspace/BadTransitive.cpp",
              "#include \"plugin/PluginThread.h\"\nvoid G(){}\n");
    WriteFile(lroot / "src/plugin/PluginHost.h", "struct PluginHost {};\n");
    WriteFile(lroot / "src/plugin/PluginThread.h",
              "// jobs are the only place a lua_State may be touched\n"
              "#include \"plugin/PluginHost.h\"\nstruct PluginThread {};\n");
    WriteFile(lroot / "src/editor/SyntaxDefinitionLoader.cpp",
              "#include <lua.hpp>\nvoid Load(lua_State* s){ (void)s; }\n");
    Expect(architecture::CheckLuaStaysBehindPluginBoundary(lroot).violations.empty(),
           "lua-boundary rule should accept Lua-free plugin facades, comment-only "
           "lua_State mentions, and the sanctioned SyntaxDefinitionLoader sandbox");
  }
}

}  // namespace

void RegisterArchitectureInvariantsTests(std::vector<TestCase>& tests) {
  // One ctest case per workspace rule so sharding parallelizes the std::regex-
  // heavy architecture lint (formerly a single ~30s serial "SoftChecks" test).
  for (const architecture::NamedRule& rule : architecture::WorkspaceArchitectureRuleList()) {
    const architecture::ArchitectureRuleFn fn = rule.fn;
    AddTest(tests, "ArchitectureInvariants/Workspace/" + std::string(rule.name),
            [fn]() { RunWorkspaceRuleTest(fn); });
  }
  AddTest(tests, "ArchitectureInvariants/PluginRules", TestArchitecturePluginRules);
  AddTest(tests, "ArchitectureInvariants/TerminalRules", TestArchitectureTerminalRules);
  AddTest(tests, "ArchitectureInvariants/FileSizes", TestArchitectureFileSizes);
  AddTest(tests, "ArchitectureInvariants/TryCatchStoScanner", TestTryCatchStoScanner);
  AddTest(tests, "ArchitectureInvariants/CountCodeLinesScanner", TestCountCodeLinesScanner);
  AddTest(tests, "ArchitectureInvariants/TargetedScannerFixtures",
          TestArchitectureInvariantTargetedScannerFixtures);
}

}  // namespace microide::tests
