#include "architecture/ArchitectureRuleFixtures.h"

#include "TestSupport.h"
#include "architecture/TerminalArchitectureRules.h"
#include "architecture/WorkspaceCoordinatorArchitectureRules.h"
#include "architecture/WorkspaceShellArchitectureRules.h"
#include "architecture/WorkspaceViewModelArchitectureRules.h"

#include <filesystem>

namespace microide::tests::architecture {

void RunDescriptorCloseOnExecRuleFixtures() {
  // A fresh root: the rule scans every descriptor-creating call under src/, so a
  // shared fixture tree would contaminate the counts.
  //
  // This fixture exists because the rule shipped with `openat?` as its `open`
  // pattern — `?` binds to the single preceding `t`, so it matched `opena` /
  // `openat` and NEVER plain `open(`. The rule therefore passed while two real
  // unflagged `open()` sites sat in the tree. Every form gets both a violating
  // and a clean fixture, so a pattern that stops matching its own call form
  // fails here instead of going quietly blind.
  TemporaryDirectory cloexec_dir;
  const std::filesystem::path& root = cloexec_dir.path();
  std::filesystem::create_directories(root / "src/platform");

  WriteFile(root / "src/platform/Descriptors.cpp",
            "int A(){ return open(\"/dev/null\", O_WRONLY); }\n"
            "int B(){ return ::openat(dir, \"f\", O_RDONLY); }\n"
            "int C(){ return ::socket(AF_UNIX, SOCK_STREAM, 0); }\n"
            "int D(){ return accept4(fd, nullptr, nullptr, 0); }\n"
            "int E(){ return accept(fd, nullptr, nullptr); }\n"
            "int F(int* p){ return pipe2(p, 0); }\n"
            "int G(){ return inotify_init1(0); }\n");
  Expect(CheckDescriptorCreationIsCloseOnExec(root).violations.size() == 7,
         "close-on-exec rule must flag every unflagged descriptor-creating form, "
         "including plain open() and plain accept()");

  // Positive control: the same call forms carrying the atomic flag must produce
  // ZERO violations. Without this half, a rule that matched nothing at all would
  // still satisfy the negative case via the loud-missing-target guard.
  WriteFile(root / "src/platform/Descriptors.cpp",
            "int A(){ return open(\"/dev/null\", O_WRONLY | O_CLOEXEC); }\n"
            "int B(){ return ::openat(dir, \"f\", O_RDONLY | O_CLOEXEC); }\n"
            "int C(){ return ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0); }\n"
            "int D(){ return accept4(fd, nullptr, nullptr, SOCK_CLOEXEC); }\n"
            "int F(int* p){ return pipe2(p, O_CLOEXEC); }\n"
            "int G(){ return inotify_init1(IN_CLOEXEC); }\n");
  Expect(CheckDescriptorCreationIsCloseOnExec(root).violations.empty(),
         "close-on-exec rule should accept every atomically-flagged creation form");

  // Occurrences inside string literals and comments are not call sites.
  WriteFile(root / "src/platform/Descriptors.cpp",
            "int G(){ return inotify_init1(IN_CLOEXEC); }\n"
            "const char* kHelp = \"open(path) and socket(fd)\";\n"
            "// accept(fd) is documented here, not called\n");
  Expect(CheckDescriptorCreationIsCloseOnExec(root).violations.empty(),
         "close-on-exec rule must code-mask string-literal and comment occurrences");

  // Loud-missing-target guard: no creation sites at all is a hollowed-out rule.
  WriteFile(root / "src/platform/Descriptors.cpp", "int H(){ return 0; }\n");
  Expect(!CheckDescriptorCreationIsCloseOnExec(root).violations.empty(),
         "close-on-exec rule must fail loudly when it finds no creation sites to scan");
}

void RunTerminalExtractedImplRuleFixtures() {
  // Five of this rule's patterns anchor a definition with `^`. std::regex only
  // treats `^` as a line start when constructed with std::regex::multiline;
  // without it `^` matches offset 0 of the whole file, which in a .cpp is always
  // `#include` — so three of the five sub-checks could never fire and the other
  // two only fired for one return-type spelling. Both halves below are needed:
  // the violating fixture proves the patterns match a real definition, and the
  // clean fixture proves they do not match the extracted-out shape.
  TemporaryDirectory impl_dir;
  const std::filesystem::path& root = impl_dir.path();
  std::filesystem::create_directories(root / "src/terminal");

  WriteFile(root / "src/terminal/TerminalSession.cpp",
            "#include \"terminal/TerminalSession.h\"\n"
            "namespace {\n"
            "}\n"
            "int Base64Value(char c) { return c; }\n"
            "std::optional<std::string> DecodeBase64(std::string_view s) { return {}; }\n"
            "std::vector<int> ParseCsiParameters(std::string_view s) { return {}; }\n"
            "int MouseModifierBits(int m) { return m; }\n"
            "bool EncodeTerminalMouseEvent(int a, std::string* out) { return false; }\n");
  Expect(CheckTerminalSessionNoExtractedImpl(root).violations.size() == 5,
         "extracted-impl rule must flag every helper implementation defined at a line "
         "start in TerminalSession.cpp");

  WriteFile(root / "src/terminal/TerminalSession.cpp",
            "#include \"terminal/TerminalSession.h\"\n"
            "#include \"terminal/TerminalBase64.h\"\n"
            "#include \"terminal/TerminalCsiParser.h\"\n"
            "#include \"terminal/TerminalMouseEncoder.h\"\n"
            "void TerminalSession::F() { (void)DecodeBase64(\"\"); (void)ParseCsiParameters(\"\"); }\n");
  Expect(CheckTerminalSessionNoExtractedImpl(root).violations.empty(),
         "extracted-impl rule must accept calls into the extracted helpers");
}

void RunDirectGitRepositoryRuleFixtures() {
  // The original pattern was `GitRepository\s*\(`, which only matches a
  // temporary. Every construction in the tree is the named-declaration form
  // (`const project::GitRepository repo(root);`), so the rule was structurally
  // unable to fire. The declaration forms below are the negative control; the
  // reference/pointer/qualified-name forms are the false-positive control.
  TemporaryDirectory git_dir;
  const std::filesystem::path& root = git_dir.path();
  std::filesystem::create_directories(root / "src/workspace");

  WriteFile(root / "src/workspace/SomeCoordinator.cpp",
            "void A(const Path& p){ project::GitRepository repo(p); (void)repo; }\n"
            "void B(const Path& p){ const GitRepository repo(p); (void)repo; }\n"
            "void C(const Path& p){ GitRepository repo{p}; (void)repo; }\n"
            "void D(const Path& p){ (void)GitRepository(p).Status(); }\n");
  Expect(CheckNoDirectGitRepositoryInWorkspace(root).violations.size() == 4,
         "direct-GitRepository rule must flag named declarations and brace init, not "
         "just a temporary");

  // The sanctioned service TU is allowlisted, and neither a reference/pointer
  // parameter, a qualified static call, nor a differently-named type is a
  // construction.
  WriteFile(root / "src/workspace/GitRepositoryService.cpp",
            "void S(const Path& p){ const project::GitRepository repo(p); (void)repo; }\n");
  WriteFile(root / "src/workspace/SomeCoordinator.cpp",
            "void A(project::GitRepository& repo){ (void)repo; }\n"
            "void B(GitRepository* repo){ (void)repo; }\n"
            "void C(){ GitRepositoryService svc(deps); (void)svc; }\n"
            "void D(){ (void)GitRepositoryState{}; }\n"
            "void E(){ (void)project::GitRepository::IsRepositoryRoot(p); }\n");
  Expect(CheckNoDirectGitRepositoryInWorkspace(root).violations.empty(),
         "direct-GitRepository rule must accept the allowlisted service TU, reference and "
         "pointer parameters, sibling types, and qualified static calls");
}

void RunActionIdReachabilityRuleFixtures() {
  // An ActionId is dispatched and gated by `switch` statements, so an action can
  // be fully implemented, compile cleanly, appear in three switches, and still be
  // impossible to invoke because nothing PRODUCES it. Four actions were in that
  // state: ToggleFullscreen (which reached SDL_SetWindowFullscreen), the two
  // DebugBreakpointEdit* modifiers (hit-count breakpoints and logpoints had no
  // menu entry), and the retired InlineCompletion.
  TemporaryDirectory action_dir;
  const std::filesystem::path& root = action_dir.path();
  std::filesystem::create_directories(root / "src/workspace");

  const char* kEnum =
      "enum class ActionId {\n"
      "  Reachable,\n"
      "  Orphan,\n"
      "  ContextMenuOnly,\n"
      "  Commented,  // Orphan is named here in a comment only\n"
      "};\n";
  WriteFile(root / "src/workspace/WorkspaceActionTypes.h", kEnum);
  // Too few enumerators: the parse-shape guard must fire rather than silently
  // reporting every action reachable.
  Expect(!CheckEveryActionIdIsReachable(root).violations.empty(),
         "a too-small ActionId parse must fail loudly, not pass vacuously");

  // Pad the enum past the vacuity floor, then give each action a distinct fate.
  std::string enum_text = "enum class ActionId {\n  Reachable,\n  Orphan,\n  ContextMenuOnly,\n";
  for (int i = 0; i < 25; ++i) {
    enum_text += "  Padding" + std::to_string(i) + ",\n";
  }
  enum_text += "};\n";
  WriteFile(root / "src/workspace/WorkspaceActionTypes.h", enum_text);

  std::string uses =
      "void Menu(){ MenuItem(ActionId::Reachable); }\n"
      "void Ctx(){ Dispatch(ActionId::ContextMenuOnly); }\n"
      "void Handle(ActionId id){\n"
      "  switch (id) {\n"
      "    case ActionId::Reachable:\n"
      "    case ActionId::Orphan:\n"
      "    case ActionId::ContextMenuOnly:\n"
      "      return;\n"
      "  }\n"
      "}\n";
  for (int i = 0; i < 25; ++i) {
    uses += "void P" + std::to_string(i) + "(){ Bind(ActionId::Padding" + std::to_string(i) + "); }\n";
  }
  WriteFile(root / "src/workspace/Uses.cpp", uses);

  const RuleResult flagged = CheckEveryActionIdIsReachable(root);
  Expect(flagged.violations.size() == 1,
         "exactly the action named only in `case` labels must be flagged");
  Expect(flagged.violations.front().message.find("Orphan") != std::string::npos,
         "the flagged action must be the orphan, not the menu- or context-menu-produced ones");

  // Positive control: giving the orphan any producer clears it. A context-menu
  // call site counts, so documented context-menu-only actions need no allowlist.
  WriteFile(root / "src/workspace/Uses.cpp", uses + "void Fix(){ Dispatch(ActionId::Orphan); }\n");
  Expect(CheckEveryActionIdIsReachable(root).violations.empty(),
         "any non-`case` mention makes an action reachable");
}

void RunRegisteredSettingsAreReadRuleFixtures() {
  // The mirror of the reads-must-be-registered rule. "Hover Delay (ms)" and
  // "Scrollbar Size" both shipped declared-and-unread: shown in the overlay,
  // persisted on change, consumed by nothing.
  TemporaryDirectory settings_dir;
  const std::filesystem::path& root = settings_dir.path();
  std::filesystem::create_directories(root / "src/workspace");

  std::string registry = "std::span<const SettingSpec> BuiltinSettingSpecs() {\n";
  for (int i = 0; i < 24; ++i) {
    registry += "  SettingSpec{ .id = \"group" + std::to_string(i) + ".used_key\" },\n";
  }
  registry += "  SettingSpec{ .id = \"group.orphan_key\" },\n};\n";
  WriteFile(root / "src/workspace/WorkspaceSettingsRegistry.cpp", registry);

  std::string reader;
  for (int i = 0; i < 24; ++i) {
    reader += "bool R" + std::to_string(i) + "(){ return SettingFlagEnabled(cfg, \"group" +
              std::to_string(i) + ".used_key\"); }\n";
  }
  WriteFile(root / "src/workspace/Reader.cpp", reader);
  const RuleResult flagged = CheckRegisteredSettingsAreRead(root);
  Expect(flagged.violations.size() == 1,
         "exactly the setting no source reads must be flagged");
  Expect(flagged.violations.front().message.find("group.orphan_key") != std::string::npos,
         "the flagged setting must be the unread one");

  // Positive control: give it a reader and the rule clears.
  WriteFile(root / "src/workspace/Reader.cpp",
            reader + "bool Orphan(){ return SettingFlagEnabled(cfg, \"group.orphan_key\"); }\n");
  Expect(CheckRegisteredSettingsAreRead(root).violations.empty(),
         "a setting with any consumer passes");

  // Vacuity guard: too few parsed ids means the declaration shape moved.
  WriteFile(root / "src/workspace/WorkspaceSettingsRegistry.cpp",
            "std::span<const SettingSpec> BuiltinSettingSpecs() { return {}; }\n");
  Expect(!CheckRegisteredSettingsAreRead(root).violations.empty(),
         "a registry the scan cannot parse must fail loudly, not pass vacuously");
}

void RunMissingRuleTargetFixtures() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path();

  // An empty tree: the rule's target does not exist, so it can scan nothing.
  // Before the guard this returned an empty result and read as a clean pass.
  const RuleResult absent = CheckEditorViewModelStickyAndOccurrenceAreSpans(root);
  Expect(!absent.missing_targets.empty(),
         "a rule whose target file is absent must report the missing target, not pass silently");
  Expect(absent.violations.empty(),
         "a missing target is a broken rule, not a code violation -- the two must stay separate");

  // Positive control: with the target present and satisfying the rule, nothing is
  // reported on either channel. Without this half, a guard that always reported a
  // missing target would still satisfy the case above.
  WriteFile(root / "src/editor/EditorViewModel.h",
            "struct EditorViewModel {\n"
            "  std::span<const std::size_t> sticky_lines;\n"
            "  std::span<const OccurrenceRange> occurrence_ranges;\n"
            "};\n");
  const RuleResult present = CheckEditorViewModelStickyAndOccurrenceAreSpans(root);
  Expect(present.missing_targets.empty(),
         "a present target must not be reported as missing");
  Expect(present.violations.empty(),
         "a present target satisfying the rule must produce no violations");
}

}  // namespace microide::tests::architecture
