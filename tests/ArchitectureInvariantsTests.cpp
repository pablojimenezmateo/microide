#include "TestSupport.h"

#include "architecture/ArchitectureFileScanner.h"
#include "architecture/ArchitectureRuleFixtures.h"
#include "architecture/ArchitectureRuleHelpers.h"
#include "architecture/KernelArchitectureRules.h"
#include "architecture/PluginArchitectureRules.h"
#include "architecture/TerminalArchitectureRules.h"
#include "architecture/WorkspaceArchitectureRules.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace microide::tests {
namespace {

void AssertRuleResultsPass(const std::vector<architecture::RuleResult>& results) {
  bool hard_failure = false;
  for (const architecture::RuleResult& result : results) {
    architecture::ReportRule(result);
    if (result.hard_fail && !result.violations.empty()) {
      hard_failure = true;
    }
    if (!result.missing_targets.empty()) {
      hard_failure = true;
    }
  }
  Expect(!hard_failure,
         "hard-fail architecture invariants should have no violations and no missing targets");
}

void RunWorkspaceRuleTest(architecture::ArchitectureRuleFn fn) {
  AssertRuleResultsPass({fn(architecture::RepoRoot())});
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
    return architecture::CheckShellFileSize(root, "src/workspace/shell/WorkspaceShell.h", 400);
  });
  run_rule("CheckShellFileSize(WorkspaceShell.cpp)", [&](const std::filesystem::path& root) {
    return architecture::CheckShellFileSize(root, "src/workspace/shell/WorkspaceShell.cpp", 600);
  });
  run_rule("CheckShellFileSize(WorkspaceShellTestAccess.h)",
           [&](const std::filesystem::path& root) {
             return architecture::CheckShellFileSize(root, "src/workspace/shell/WorkspaceShellTestAccess.h",
                                                     600);
           });
  run_rule("CheckShellFileSize(WorkspaceShellMembers.inc)",
           [&](const std::filesystem::path& root) {
             return architecture::CheckShellFileSize(root, "src/workspace/shell/WorkspaceShellMembers.inc",
                                                     1426);
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
      "\n"
      "   \t  \n"
      "/* block-only line */\n"
      "const char* s = \"// not a comment\";\n"
      "}  // counted: brace with trailing comment\n"
      "int y = 2;";
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
  WriteFile(root / "src/workspace/render/WorkspaceShellRenderSidebar.cpp",
            "std::string F(){ return std::string(\"search> \") + std::string(\"x\"); }\n");
  WriteFile(root / "src/workspace/WorkspaceLanguageContract.cpp", "// lang contract fixture\n");
  WriteFile(root / "src/editor/IndentGuides.cpp", "// indent guides fixture\n");
  WriteFile(root / "src/editor/SnippetEngine.cpp", "// snippet engine fixture\n");
  WriteFile(root / "src/editor/FoldingModel.cpp", "void leak(lua_State* L){ (void)L; }\n");
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
  WriteFile(root / "src/workspace/shell/WorkspaceShellOverlay.cpp",
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

  WriteFile(root / "src/workspace/render/RenderViewModelBuilder.cpp",
            "int Parse() { return static_cast<int>(std::stol(\"3\")); }\n");
  Expect(!architecture::CheckNoStdStoInRenderOrBuilderTus(root).violations.empty(),
         "std::sto* rule should catch std::stol in RenderViewModelBuilder.cpp");
  WriteFile(root / "src/workspace/render/RenderViewModelBuilder.cpp",
            "int Parse() { return util::ParseInt(\"3\").value_or(3); }\n");
  Expect(architecture::CheckNoStdStoInRenderOrBuilderTus(root).violations.empty(),
         "std::sto* rule should accept the util::ParseInt rewrite");

  WriteFile(root / "src/workspace/shell/WorkspaceShellPresentation.cpp",
            "std::string F(const Repo& repo) { return repo.Execute({\"symbolic-ref\"}).output; }\n");
  WriteFile(root / "src/workspace/services/StatusBarModelService.cpp", "// clean model-build fixture\n");
  Expect(!architecture::CheckStatusBarRefreshIsAsyncOnly(root).violations.empty(),
         "status-bar rule should catch synchronous repo.Execute in the frame-path TU");
  WriteFile(root / "src/workspace/shell/WorkspaceShellPresentation.cpp",
            "std::string F() { return std::string(\"async-only\"); }\n");
  Expect(architecture::CheckStatusBarRefreshIsAsyncOnly(root).violations.empty(),
         "status-bar rule should pass on async-only fixtures for both target TUs");
  std::filesystem::remove(root / "src/workspace/services/StatusBarModelService.cpp");
  Expect(!architecture::CheckStatusBarRefreshIsAsyncOnly(root).violations.empty(),
         "a moved/renamed status-bar rule target must fail loudly, not pass vacuously");
  WriteFile(root / "src/workspace/services/StatusBarModelService.cpp", "// clean model-build fixture\n");

  WriteFile(root / "src/workspace/render/RenderViewModelBuilder.h",
            "struct SidebarSurfaceViewModel { std::string query_fallback_text; "
            "std::string replace_fallback_text; };\n");
  Expect(!architecture::CheckSidebarSurfaceFallbackUsesStringView(root).violations.empty(),
         "sidebar-fallback rule should catch std::string fallback fields");
  WriteFile(root / "src/workspace/render/RenderViewModelBuilder.h",
            "struct SidebarSurfaceViewModel { std::string_view query_fallback_text; "
            "std::string_view replace_fallback_text; };\n");
  Expect(architecture::CheckSidebarSurfaceFallbackUsesStringView(root).violations.empty(),
         "sidebar-fallback rule should pass on the string_view fixture");

  WriteFile(root / "src/workspace/render/WorkspaceShellRenderSidebar.cpp",
            "void F(char m, const std::string& s){ auto x = std::string(1, m); "
            "auto y = std::string(\"foo: \") + s; (void)x; (void)y; }\n");
  Expect(!architecture::CheckRenderTuDoesNotMaterializeSingleCharOrPrefixStrings(root).violations.empty(),
         "render-string rule should catch single-char std::string and literal+ident concat");
  WriteFile(root / "src/workspace/render/WorkspaceShellRenderSidebar.cpp",
            "void F(char m, const std::string& s){ std::string_view x(&m, 1); (void)x; (void)s; }\n");
  Expect(architecture::CheckRenderTuDoesNotMaterializeSingleCharOrPrefixStrings(root).violations.empty(),
         "render-string rule should pass on the string_view rewrite fixture");

  WriteFile(root / "src/workspace/render/WorkspaceShellRenderSidebar.cpp",
            "void F(const std::string& q){ auto s = \"search> \" + q; (void)s; }\n");
  Expect(!architecture::CheckRenderTuDoesNotMaterializeStrings(root).violations.empty(),
         "render fallback rule should catch the bare \"search> \" + ident concat form");
  WriteFile(root / "src/workspace/render/WorkspaceShellRenderSidebar.cpp",
            "void F(std::string_view fallback){ DrawText(fallback); }\n");
  Expect(architecture::CheckRenderTuDoesNotMaterializeStrings(root).violations.empty(),
         "render fallback rule should pass when the fallback uses a precomputed view");

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

  WriteFile(root / "src/workspace/render/RenderViewModelBuilder.h",
            "struct OverlaySurfaceViewModel { const OverlayState* state = nullptr; "
            "ProjectWorkspaceState* project_state = nullptr; };\n");
  Expect(architecture::CheckRenderViewModelsOwnProjectState(root).violations.size() == 2,
         "view-model state rule should catch both the OverlayState pointer and the "
         "non-allowlisted ProjectWorkspaceState pointer");
  WriteFile(root / "src/workspace/render/WorkspaceShellRenderOverlay.cpp",
            "void F(const ProjectWorkspaceState& s){ (void)s; }\n");
  WriteFile(root / "src/workspace/render/DebugPaneRender.cpp",
            "void G(const OverlayState& s){ (void)s; }\n");
  Expect(architecture::CheckRenderViewModelsOwnProjectState(root).violations.size() == 4,
         "view-model state rule should catch broad state type names in converted render TUs");
  WriteFile(root / "src/workspace/render/RenderViewModelBuilder.h",
            "struct FrameSurfaceViewModel { ProjectWorkspaceState* project_state = nullptr; };\n"
            "struct SidebarSurfaceViewModel { std::string_view query_fallback_text; "
            "std::string_view replace_fallback_text; "
            "ProjectWorkspaceState* project_state = nullptr; };\n"
            "struct OverlaySurfaceViewModel { std::string label_storage; };\n");
  WriteFile(root / "src/workspace/render/WorkspaceShellRenderOverlay.cpp",
            "void F(const OverlaySurfaceViewModel& vm){ (void)vm; }\n");
  WriteFile(root / "src/workspace/render/DebugPaneRender.cpp",
            "void G(const DebugPaneSurfaceViewModel& vm){ (void)vm; }\n");
  Expect(architecture::CheckRenderViewModelsOwnProjectState(root).violations.empty(),
         "view-model state rule should pass on the owned-model + allowlisted-structs fixture");

  WriteFile(root / "src/workspace/SomeStateSaver.cpp",
            "void Save(const std::filesystem::path& p){ std::ofstream out(p); out << 1; }\n");
  Expect(!architecture::CheckPersistenceFileIoBoundary(root).violations.empty(),
         "persistence I/O rule should catch a raw ofstream in a non-allowlisted workspace TU");
  WriteFile(root / "src/workspace/SomeStateSaver.cpp",
            "void Save(persistence::PersistedRecordWriter& writer){ writer.Commit(); }\n");
  WriteFile(root / "src/workspace/persistence/PersistenceService.cpp",
            "void G(const std::filesystem::path& p){ std::ifstream in(p); (void)in; }\n");
  Expect(architecture::CheckPersistenceFileIoBoundary(root).violations.empty(),
         "persistence I/O rule should accept the record-writer rewrite and the sanctioned "
         "PersistenceService TU");

  WriteFile(root / "src/workspace/services/ProjectCatalogService.cpp",
            "void Reactivate(){ ReloadPluginsForCurrentProject(); }\n");
  Expect(!architecture::CheckReactivationDoesNotReloadPlugins(root).violations.empty(),
         "reactivation rule should catch a plugin reload in the reactivation TU");
  WriteFile(root / "src/workspace/services/ProjectCatalogService.cpp",
            "void Reactivate(){ operations_.refresh_plugin_surfaces_for_reactivation(); }\n");
  Expect(architecture::CheckReactivationDoesNotReloadPlugins(root).violations.empty(),
         "reactivation rule should accept the refresh-seam rewrite");
  std::filesystem::remove(root / "src/workspace/services/ProjectCatalogService.cpp");
  Expect(!architecture::CheckReactivationDoesNotReloadPlugins(root).violations.empty(),
         "a moved reactivation TU must fail loudly, not pass vacuously");
  WriteFile(root / "src/workspace/services/ProjectCatalogService.cpp",
            "void Reactivate(){ operations_.refresh_plugin_surfaces_for_reactivation(); }\n");

  WriteFile(root / "src/workspace/SomeShellState.h",
            "struct S { editor::TextViewport* text_viewport_ = nullptr; };\n");
  Expect(!architecture::CheckNoFallbackEditorViewportSymbols(root).violations.empty(),
         "viewport-symbol rule should catch a revived fallback viewport member");
  WriteFile(root / "src/workspace/SomeShellState.h",
            "struct S { };  // active viewport resolves through EditorTabService\n");
  Expect(architecture::CheckNoFallbackEditorViewportSymbols(root).violations.empty(),
         "viewport-symbol rule should pass once the member is gone");

  {
    TemporaryDirectory lua_dir;
    const auto& lroot = lua_dir.path();
    std::filesystem::create_directories(lroot / "src/plugin");
    std::filesystem::create_directories(lroot / "src/workspace");
    std::filesystem::create_directories(lroot / "src/editor");
    WriteFile(lroot / "src/workspace/BadDirect.cpp",
              "#include <lua.hpp>\nvoid F(lua_State* s){ (void)s; }\n");
    WriteFile(lroot / "src/plugin/Exposed.h", "#include <lua.hpp>\n");
    WriteFile(lroot / "src/plugin/CleanFacade.h", "#include \"plugin/Exposed.h\"\n");
    WriteFile(lroot / "src/workspace/BadTransitive.cpp",
              "#include \"plugin/CleanFacade.h\"\nvoid G(){}\n");
    const auto violating = architecture::CheckLuaStaysBehindPluginBoundary(lroot);
    Expect(violating.violations.size() == 3,
           "lua-boundary rule should catch the lua_State token, the Lua include, and the "
           "transitively Lua-exposing plugin include");

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

  architecture::RunAllRuleFixtures();
}

}

void RegisterArchitectureInvariantsTests(std::vector<TestCase>& tests) {
  const std::pair<std::string_view, const std::vector<architecture::NamedRule>&> groups[] = {
      {"Workspace", architecture::WorkspaceArchitectureRuleList()},
      {"Plugin", architecture::PluginArchitectureRuleList()},
      {"Terminal", architecture::TerminalArchitectureRuleList()},
      {"Kernel", architecture::KernelArchitectureRuleList()},
  };
  for (const auto& [group, rules] : groups) {
    for (const architecture::NamedRule& rule : rules) {
      const architecture::ArchitectureRuleFn fn = rule.fn;
      AddTest(tests, "ArchitectureInvariants/" + std::string(group) + "/" + std::string(rule.name),
              [fn]() { RunWorkspaceRuleTest(fn); });
    }
  }
  AddTest(tests, "ArchitectureInvariants/FileSizes", TestArchitectureFileSizes);
  AddTest(tests, "ArchitectureInvariants/TryCatchStoScanner", TestTryCatchStoScanner);
  AddTest(tests, "ArchitectureInvariants/CountCodeLinesScanner", TestCountCodeLinesScanner);
  AddTest(tests, "ArchitectureInvariants/TargetedScannerFixtures",
          TestArchitectureInvariantTargetedScannerFixtures);
}

}
