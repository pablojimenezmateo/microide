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
             return architecture::CheckShellFileSize(root, "src/workspace/WorkspaceShellMembers.inc",
                                                     1519);
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
  WriteFile(root / "src/workspace/WorkspaceDapManager.cpp", "void X(){ DapManager* m = nullptr; }\n");

  Expect(!architecture::CheckEssentialEditorCppModulesDoNotTouchLuaState(root).violations.empty(),
         "lua_State pointers should not appear in FoldingModel.cpp fixture");

  Expect(!architecture::CheckNoLegacyPersistenceSymbols(root).violations.empty(),
         "legacy-persistence rule should catch legacy symbols");
  Expect(!architecture::CheckNoDebuggerDapSurface(root).violations.empty(),
         "debugger/DAP rule should catch removed surface symbols");
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
  AddTest(tests, "ArchitectureInvariants/TargetedScannerFixtures",
          TestArchitectureInvariantTargetedScannerFixtures);
}

}  // namespace microide::tests
