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

void TestArchitectureInvariants() {
  const std::filesystem::path repo_root = architecture::RepoRoot();
  std::vector<architecture::RuleResult> results;
  const auto run_rule = [&](const char* label, auto&& fn) {
    try {
      results.push_back(fn(repo_root));
    } catch (const std::exception& error) {
      throw std::runtime_error(std::string(label) + ": " + error.what());
    }
  };

  for (architecture::RuleResult& result : architecture::RunWorkspaceArchitectureRules(repo_root)) {
    results.push_back(std::move(result));
  }
  for (architecture::RuleResult& result : architecture::RunPluginArchitectureRules(repo_root)) {
    results.push_back(std::move(result));
  }
  for (architecture::RuleResult& result : architecture::RunTerminalArchitectureRules(repo_root)) {
    results.push_back(std::move(result));
  }

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
             return architecture::CheckShellFileSize(root, "src/workspace/WorkspaceShellMembers.inc",
                                                     1658);
           });

  bool hard_failure = false;
  for (const architecture::RuleResult& result : results) {
    architecture::ReportRule(result);
    if (result.hard_fail && !result.violations.empty()) {
      hard_failure = true;
    }
  }

  Expect(!hard_failure, "hard-fail architecture invariants should have no violations");
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
  WriteFile(root / "src/editor/TextViewport.cpp",
            "std::size_t TextViewport::ReplaceAll(std::string_view, std::string_view) {\n"
            "  std::vector<std::string> before = document_->lines;\n"
            "  (void)before;\n"
            "  return 0;\n"
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
  Expect(!architecture::CheckRenderTuDoesNotMaterializeStrings(root).violations.empty(),
         "render materialization rule should catch string construction in render TU");
  Expect(!architecture::CheckTextViewportNoFullDocCopy(root).violations.empty(),
         "TextViewport rule should catch full document copies");

  WriteFile(root / "src/editor/TextViewport.cpp",
            "std::size_t TextViewport::ReplaceAll(std::string_view, std::string_view) {\n"
            "  return 0;\n"
            "}\n"
            "bool TextViewport::ApplyLineEdit(std::size_t,std::size_t,const "
            "std::vector<std::string>&) {\n"
            "  auto snap = document_->lines;\n"
            "  return !snap.empty();\n"
            "}\n");
  Expect(!architecture::CheckTextViewportApplyPipelineNoFullDocumentLineSnapshot(root).violations.empty(),
         "ApplyLineEdit fixture should flag full document_->lines snapshots");

  WriteFile(root / "src/workspace/RenderViewModelBuilder.cpp",
            "int Parse() { return static_cast<int>(std::stol(\"3\")); }\n");
  Expect(!architecture::CheckNoStdStoInRenderOrBuilderTus(root).violations.empty(),
         "std::sto* rule should catch std::stol in RenderViewModelBuilder.cpp");
  WriteFile(root / "src/workspace/RenderViewModelBuilder.cpp",
            "int Parse() { return util::ParseInt(\"3\").value_or(3); }\n");
  Expect(architecture::CheckNoStdStoInRenderOrBuilderTus(root).violations.empty(),
         "std::sto* rule should accept the util::ParseInt rewrite");

  WriteFile(root / "src/workspace/WorkspaceShellChrome.cpp",
            "std::string F(const Repo& repo) { return repo.Execute({\"symbolic-ref\"}).output; }\n");
  Expect(!architecture::CheckStatusBarRefreshIsAsyncOnly(root).violations.empty(),
         "status-bar rule should catch synchronous repo.Execute in WorkspaceShellChrome.cpp");
  WriteFile(root / "src/workspace/WorkspaceShellChrome.cpp",
            "std::string F() { return std::string(\"async-only\"); }\n");
  Expect(architecture::CheckStatusBarRefreshIsAsyncOnly(root).violations.empty(),
         "status-bar rule should pass on async-only fixture");

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
}

}  // namespace

void RegisterArchitectureInvariantsTests(std::vector<TestCase>& tests) {
  AddTest(tests, "ArchitectureInvariants/SoftChecks", TestArchitectureInvariants);
  AddTest(tests, "ArchitectureInvariants/TryCatchStoScanner", TestTryCatchStoScanner);
  AddTest(tests, "ArchitectureInvariants/CountCodeLinesScanner", TestCountCodeLinesScanner);
  AddTest(tests, "ArchitectureInvariants/TargetedScannerFixtures",
          TestArchitectureInvariantTargetedScannerFixtures);
}

}  // namespace microide::tests
