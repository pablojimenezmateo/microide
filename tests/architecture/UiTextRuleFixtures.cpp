#include "architecture/ArchitectureRuleFixtures.h"

#include "TestSupport.h"
#include "architecture/WorkspaceViewModelArchitectureRules.h"

#include <filesystem>
#include <string>

namespace microide::tests::architecture {

void RunSideNameRuleFixtures() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path();
  std::filesystem::create_directories(root / "src/workspace/services");
  WriteFile(root / "src/workspace/services/RemoteHostService.cpp",
            "const char* a = \"Keep Local\"; const char* b = \"Take Remote\";\n");
  const std::filesystem::path probe = root / "src/workspace/Probe.cpp";
  const auto flags = [&](const std::string& source) {
    WriteFile(probe, source);
    return !CheckSideNamesAreLocalAndRemote(root).violations.empty();
  };

  Expect(flags("const char* k = \"Keep Mine\";\n"), "the side rule must flag Keep Mine");
  Expect(flags("const char* k = \"Take All Host's\";\n"), "the side rule must flag Take All Host's");
  Expect(flags("const char* k = \"Accept Theirs\";\n"), "the side rule must flag Accept Theirs");
  Expect(flags("const char* k = \"Git: Accept Current (Keep Ours)\";\n"),
         "the side rule must flag a palette title naming Current / Ours");
  Expect(flags("std::string t = name + \" (yours)\";\n"), "the side rule must flag a (yours) suffix");
  Expect(flags("draw(r, \"Theirs\");\n"), "the side rule must flag a bare Theirs caption");

  WriteFile(probe,
            "const char* a = \"Accept Remote\"; const char* b = \"main.c (local)\";\n"
            "const char* c = \"Your ~/.ssh/config applies.\";\n"
            "const char* d = \"please commit your changes or stash them\";\n"
            "const char* e = \"Incoming Calls\";\n"
            "// was \"Keep Mine\" until 2026-10-29\n");
  {
    const RuleResult clean = CheckSideNamesAreLocalAndRemote(root);
    Expect(clean.violations.empty() && clean.missing_targets.empty(),
           "the side rule must pass Local/Remote labels, prose and comments");
  }

  WriteFile(root / "src/workspace/services/RemoteHostService.cpp", "int x = 0;\n");
  WriteFile(probe, "int y = 0;\n");
  Expect(!CheckSideNamesAreLocalAndRemote(root).violations.empty(),
         "the side rule must report its anchor moved");
}

void RunDerivedSurfacePaintRuleFixtures() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path();
  const std::filesystem::path file = root / "src/workspace/render/WorkspaceShellRenderStatusBar.cpp";
  std::filesystem::create_directories(file.parent_path());
  const auto flags = [&](const std::string& body) {
    WriteFile(file, "void WorkspaceShell::RenderStatusBar(SDL_Renderer* r, const WorkspaceLayout& l) const {\n" +
                        body + "\n}\nvoid Other() { context_.x(); }\n");
    return !CheckDerivedSurfacePaintReadsOnlyItsSnapshot(root).violations.empty();
  };
  Expect(flags("  auto vm = RenderViewModelBuilder(context_).BuildStatusBar(l, status_bar_service_);"),
         "the derived-paint rule must flag a view model built in paint");
  Expect(flags("  if (PointerOver(rect)) {}"), "the derived-paint rule must flag the live pointer");
  Expect(flags("  const auto t = SDL_GetTicks();"), "the derived-paint rule must flag the clock");
  Expect(flags("  MakeEditorTabService().Active();"), "the derived-paint rule must flag a service");
  Expect(!flags("  const PreparedFrameState& frame = frame_state_;  // not context_ here\n"
                "  DrawFilledRect(r, frame.status_bar.rect, theme_.chrome_background);"),
         "the derived-paint rule must pass a paint that reads only the prepared frame");
  const std::filesystem::path helpers = root / "src/workspace/render/ProbeHelpers.cpp";
  const auto calls = [&](const std::string& helper_text) {
    WriteFile(helpers, helper_text);
    WriteFile(file, "void WorkspaceShell::RenderStatusBar(SDL_Renderer* r, const WorkspaceLayout& l) const {\n"
                    "  DrawProbe(r);\n}\n");
    const bool flagged = !CheckDerivedSurfacePaintReadsOnlyItsSnapshot(root).violations.empty();
    std::filesystem::remove(helpers);
    return flagged;
  };
  Expect(calls("void WorkspaceShell::DrawProbe(SDL_Renderer* r) const {\n  if (context_.prompts.dirty_visible) {}\n}\n"),
         "the derived-paint rule must flag a member the paint calls that reads live state");
  Expect(calls("void WorkspaceShell::DrawProbe(SDL_Renderer* r) const {\n  LabelOf(r);\n}\n"
               "std::string_view WorkspaceShell::LabelOf(SDL_Renderer*) const {\n"
               "  return editor_blame_overlay_service_.Label();\n}\n"),
         "the derived-paint rule must flag a live read two calls down");
  Expect(!calls("void WorkspaceShell::DrawProbe(SDL_Renderer* r) const {\n"
                "  DrawFilledRect(r, frame_state_.status_bar.rect, theme_.border);  // not context_\n}\n"
                "void Unrelated() { context_.x(); }\n"),
         "the derived-paint rule must pass a member the paint calls that reads only the prepared frame");
  const std::filesystem::path header = root / "src/workspace/render/ProbeInputs.h";
  const auto handed = [&](const std::string& parameters, const std::string& header_text) {
    WriteFile(header, header_text);
    WriteFile(file, "void WorkspaceShell::RenderStatusBar(" + parameters + ") const {\n  Draw(r);\n}\n");
    return !CheckDerivedSurfacePaintReadsOnlyItsSnapshot(root).violations.empty();
  };
  Expect(handed("SDL_Renderer* r, const editor::DiagnosticsStore& diagnostics", ""),
         "the derived-paint rule must flag a store passed as a parameter");
  Expect(handed("SDL_Renderer* r, EditorTabService* tabs", ""),
         "the derived-paint rule must flag a service passed as a parameter");
  Expect(handed("SDL_Renderer* r, const ProbeInputs& inputs",
                "struct ProbeInputs {\n  const editor::DiagnosticsStore* diagnostics_store = nullptr;\n};\n"),
         "the derived-paint rule must flag a store reached through an inputs struct");
  Expect(handed("SDL_Renderer* r, const ProbeInputs& inputs",
                "struct ProbeInputs {\n  const ProbePaint* paint = nullptr;\n};\n"
                "struct ProbePaint {\n  bool focused = false;\n  GitBlameService* blame = nullptr;\n};\n"),
         "the derived-paint rule must flag a service two structs down");
  Expect(handed("SDL_Renderer* r, const GhostInputs& inputs", "struct ProbeInputs {};\n"),
         "the derived-paint rule must report an inputs struct it cannot find");
  Expect(!handed("SDL_Renderer* r, const ProbeInputs& inputs, const render::Theme& theme, std::size_t pane",
                 "struct ProbeInputs {\n  // no DiagnosticsStore here: prep resolved the file's\n"
                 "  const ProbePaint* paint = nullptr;\n};\n"
                 "struct ProbePaint {\n  std::span<const editor::PublishedDiagnostic> diagnostics;\n"
                 "  const editor::FileDecorations* decorations = nullptr;\n"
                 "  std::string_view note = \"not a WorkspaceContext\";\n};\n"),
         "the derived-paint rule must pass prepared inputs that hold resolved data only");
  std::filesystem::remove(header);
  WriteFile(file, "void WorkspaceShell::RenderSomethingElse() {}\n");
  Expect(!CheckDerivedSurfacePaintReadsOnlyItsSnapshot(root).violations.empty(),
         "the derived-paint rule must report a migrated function it cannot find");
  std::filesystem::remove(file);
  const RuleResult gone = CheckDerivedSurfacePaintReadsOnlyItsSnapshot(root);
  Expect(!gone.missing_targets.empty(), "the derived-paint rule must report a missing file");
}

void RunHintSeparatorRuleFixtures() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path();
  std::filesystem::create_directories(root / "src/workspace/git");

  WriteFile(root / "src/workspace/WorkspaceUiText.h",
            "inline constexpr std::string_view kHintSeparator = \" \\u00b7 \";\n"
            "void AppendHintSegment(std::string& line, std::string_view s);\n");

  const std::filesystem::path probe = root / "src/workspace/git/GitSidebarCommandCenter.cpp";

  WriteFile(probe, "void AppendHintSegment(std::string& l, std::string_view s) {}\n");
  Expect(!CheckHintSegmentsUseTheSharedSeparator(root).violations.empty(),
         "the hint rule must still flag a redefined AppendHintSegment");

  WriteFile(probe, "const char* k = \"Enter default  |  r refresh\";\n");
  Expect(!CheckHintSegmentsUseTheSharedSeparator(root).violations.empty(),
         "the hint rule must flag a key-hint separator typed inline into a literal");

  WriteFile(probe, "std::string F(std::string a, std::string b){ return a + \"  |  \" + b; }\n");
  {
    const RuleResult bare = CheckHintSegmentsUseTheSharedSeparator(root);
    Expect(bare.violations.empty() && bare.missing_targets.empty(),
           "the hint rule must pass a bare separator literal joined at runtime");
  }

  WriteFile(probe, "const char* k = \"Enter default \\u00b7 r refresh\";\n");
  {
    const RuleResult fixed = CheckHintSegmentsUseTheSharedSeparator(root);
    Expect(fixed.violations.empty() && fixed.missing_targets.empty(),
           "the hint rule must pass a composed line that uses the shared separator");
  }

  WriteFile(probe, "// was \"Enter default  |  r refresh\" until 2026-09-19\nint k = 0;\n");
  {
    const RuleResult commented = CheckHintSegmentsUseTheSharedSeparator(root);
    Expect(commented.violations.empty() && commented.missing_targets.empty(),
           "the hint rule must not flag a comment that quotes the old separator");
  }
}

}
