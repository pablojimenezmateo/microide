#include "architecture/ArchitectureRuleFixtures.h"

#include "TestSupport.h"
#include "architecture/WorkspaceViewModelArchitectureRules.h"

#include <filesystem>
#include <string>

// Fixtures for the rules about user-visible text: key-hint separators, and the
// names of a conflict's or a merge's two sides.

namespace microide::tests::architecture {

void RunSideNameRuleFixtures() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path();
  std::filesystem::create_directories(root / "src/workspace/services");
  // The anchor: the rule reports a moved target rather than passing blind.
  WriteFile(root / "src/workspace/services/RemoteHostService.cpp",
            "const char* a = \"Keep Local\"; const char* b = \"Take Remote\";\n");
  const std::filesystem::path probe = root / "src/workspace/Probe.cpp";
  const auto flags = [&](const std::string& source) {
    WriteFile(probe, source);
    return !CheckSideNamesAreLocalAndRemote(root).violations.empty();
  };

  // Negative controls: each shape the five old names took.
  Expect(flags("const char* k = \"Keep Mine\";\n"), "the side rule must flag Keep Mine");
  Expect(flags("const char* k = \"Take All Host's\";\n"), "the side rule must flag Take All Host's");
  Expect(flags("const char* k = \"Accept Theirs\";\n"), "the side rule must flag Accept Theirs");
  Expect(flags("const char* k = \"Git: Accept Current (Keep Ours)\";\n"),
         "the side rule must flag a palette title naming Current / Ours");
  Expect(flags("std::string t = name + \" (yours)\";\n"), "the side rule must flag a (yours) suffix");
  Expect(flags("draw(r, \"Theirs\");\n"), "the side rule must flag a bare Theirs caption");

  // Positive controls: the new names, and prose that only addresses the user.
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

  // The anchor gone: a violation naming the rule, not a silent pass.
  WriteFile(root / "src/workspace/services/RemoteHostService.cpp", "int x = 0;\n");
  WriteFile(probe, "int y = 0;\n");
  Expect(!CheckSideNamesAreLocalAndRemote(root).violations.empty(),
         "the side rule must report its anchor moved");
}

void RunHintSeparatorRuleFixtures() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path();
  std::filesystem::create_directories(root / "src/workspace/git");

  // The rule reads WorkspaceUiText.h as its anchor and reports it missing rather
  // than passing blind, so the fixture has to provide a plausible one.
  WriteFile(root / "src/workspace/WorkspaceUiText.h",
            "inline constexpr std::string_view kHintSeparator = \" \\u00b7 \";\n"
            "void AppendHintSegment(std::string& line, std::string_view s);\n");

  const std::filesystem::path probe = root / "src/workspace/git/GitSidebarCommandCenter.cpp";

  // Negative control for the original half: a private copy of the joiner.
  WriteFile(probe, "void AppendHintSegment(std::string& l, std::string_view s) {}\n");
  Expect(!CheckHintSegmentsUseTheSharedSeparator(root).violations.empty(),
         "the hint rule must still flag a redefined AppendHintSegment");

  // Negative control for the half added 2026-09-19: the separator typed inline
  // into a composed line. This is the shape both real defects took, and the
  // shape the redefinition-scoped rule was structurally unable to see.
  WriteFile(probe, "const char* k = \"Enter default  |  r refresh\";\n");
  Expect(!CheckHintSegmentsUseTheSharedSeparator(root).violations.empty(),
         "the hint rule must flag a key-hint separator typed inline into a literal");

  // Positive control 1: the bare separator literal, joined at runtime. This is
  // how the breadcrumb and the merge status line legitimately use "  |  " to
  // separate unrelated fields, and the rule must not fail them.
  WriteFile(probe, "std::string F(std::string a, std::string b){ return a + \"  |  \" + b; }\n");
  {
    const RuleResult bare = CheckHintSegmentsUseTheSharedSeparator(root);
    Expect(bare.violations.empty() && bare.missing_targets.empty(),
           "the hint rule must pass a bare separator literal joined at runtime");
  }

  // Positive control 2: the fix. A composed line punctuated with kHintSeparator.
  WriteFile(probe, "const char* k = \"Enter default \\u00b7 r refresh\";\n");
  {
    const RuleResult fixed = CheckHintSegmentsUseTheSharedSeparator(root);
    Expect(fixed.violations.empty() && fixed.missing_targets.empty(),
           "the hint rule must pass a composed line that uses the shared separator");
  }

  // Positive control 3: a COMMENT mentioning the wrong separator -- both real
  // fixes left one behind explaining the history, and the rule is code-masked so
  // neither may trip it.
  WriteFile(probe, "// was \"Enter default  |  r refresh\" until 2026-09-19\nint k = 0;\n");
  {
    const RuleResult commented = CheckHintSegmentsUseTheSharedSeparator(root);
    Expect(commented.violations.empty() && commented.missing_targets.empty(),
           "the hint rule must not flag a comment that quotes the old separator");
  }
}

}  // namespace microide::tests::architecture
