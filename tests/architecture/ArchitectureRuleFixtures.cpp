#include "architecture/ArchitectureRuleFixtures.h"

#include "TestSupport.h"
#include "architecture/TerminalArchitectureRules.h"
#include "architecture/WorkspaceArchitectureRules.h"
#include "architecture/WorkspaceCoordinatorArchitectureRules.h"
#include "architecture/WorkspaceServiceArchitectureRules.h"
#include "architecture/WorkspaceShellArchitectureRules.h"
#include "architecture/KernelArchitectureRules.h"
#include "architecture/LanguageHazardArchitectureRules.h"
#include "architecture/CommentArchitectureRules.h"
#include "architecture/InputGeometryArchitectureRules.h"
#include "architecture/WorkspaceViewModelArchitectureRules.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace microide::tests::architecture {

void RunDescriptorCloseOnExecRuleFixtures() {
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
            "int G(){ return inotify_init1(0); }\n"
            "int H(){ return posix_openpt(O_RDWR | O_NOCTTY); }\n"
            "int I(int* m, int* s){ return openpty(m, s, nullptr, nullptr, nullptr); }\n"
            "int J(int* m){ return forkpty(m, nullptr, nullptr, nullptr); }\n");
  Expect(CheckDescriptorCreationIsCloseOnExec(root).violations.size() == 10,
         "close-on-exec rule must flag every unflagged descriptor-creating form, "
         "including plain open(), plain accept(), openpty() and forkpty()");

  WriteFile(root / "src/platform/Descriptors.cpp",
            "int A(){ return open(\"/dev/null\", O_WRONLY | O_CLOEXEC); }\n"
            "int B(){ return ::openat(dir, \"f\", O_RDONLY | O_CLOEXEC); }\n"
            "int C(){ return ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0); }\n"
            "int D(){ return accept4(fd, nullptr, nullptr, SOCK_CLOEXEC); }\n"
            "int F(int* p){ return pipe2(p, O_CLOEXEC); }\n"
            "int G(){ return inotify_init1(IN_CLOEXEC); }\n"
            "int H(){ return posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC); }\n");
  Expect(CheckDescriptorCreationIsCloseOnExec(root).violations.empty(),
         "close-on-exec rule should accept every atomically-flagged creation form");

  WriteFile(root / "src/platform/Descriptors.cpp",
            "int G(){ return inotify_init1(IN_CLOEXEC); }\n"
            "const char* kHelp = \"open(path) and socket(fd)\";\n"
            "// accept(fd) is documented here, not called\n");
  Expect(CheckDescriptorCreationIsCloseOnExec(root).violations.empty(),
         "close-on-exec rule must code-mask string-literal and comment occurrences");

  WriteFile(root / "src/platform/Descriptors.cpp", "int H(){ return 0; }\n");
  Expect(!CheckDescriptorCreationIsCloseOnExec(root).violations.empty(),
         "close-on-exec rule must fail loudly when it finds no creation sites to scan");
}

void RunTerminalExtractedImplRuleFixtures() {
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

  WriteFile(root / "src/workspace/git/GitRepositoryService.cpp",
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
  WriteFile(root / "src/workspace/actions/WorkspaceActionTypes.h", kEnum);
  Expect(!CheckEveryActionIdIsReachable(root).violations.empty(),
         "a too-small ActionId parse must fail loudly, not pass vacuously");

  std::string enum_text =
      "enum class ActionId {\n  Reachable,\n  Orphan,\n  ContextMenuOnly,\n  NeverHandled,\n";
  for (int i = 0; i < 25; ++i) {
    enum_text += "  Padding" + std::to_string(i) + ",\n";
  }
  enum_text += "};\n";
  WriteFile(root / "src/workspace/actions/WorkspaceActionTypes.h", enum_text);

  std::string uses =
      "void Menu(){ MenuItem(ActionId::Reachable); }\n"
      "void Ctx(){ Dispatch(ActionId::ContextMenuOnly); }\n"
      "void Unhandled(){ MenuItem(ActionId::NeverHandled); }\n"
      "void Handle(ActionId id){\n"
      "  switch (id) {\n"
      "    case ActionId::Reachable:\n"
      "    case ActionId::Orphan:\n"
      "    case ActionId::ContextMenuOnly:\n"
      "      return;\n"
      "  }\n"
      "}\n";
  for (int i = 0; i < 25; ++i) {
    const std::string n = std::to_string(i);
    uses += "void P" + n + "(){ Bind(ActionId::Padding" + n + "); }\n";
    uses += "void H" + n + "(ActionId id){ switch (id) {\n    case ActionId::Padding" + n +
            ":\n      return;\n  } }\n";
  }
  WriteFile(root / "src/workspace/Uses.cpp", uses);

  const RuleResult flagged = CheckEveryActionIdIsReachable(root);
  Expect(flagged.violations.size() == 2,
         "exactly the unproduced action and the unhandled action must be flagged, got " +
             std::to_string(flagged.violations.size()));
  const bool flags_orphan =
      std::any_of(flagged.violations.begin(), flagged.violations.end(), [](const Violation& v) {
        return v.message.find("Orphan") != std::string::npos &&
               v.message.find("nothing produces it") != std::string::npos;
      });
  const bool flags_unhandled =
      std::any_of(flagged.violations.begin(), flagged.violations.end(), [](const Violation& v) {
        return v.message.find("NeverHandled") != std::string::npos &&
               v.message.find("never appears as a `case") != std::string::npos;
      });
  Expect(flags_orphan, "the case-only action must be flagged as unproduced");
  Expect(flags_unhandled, "the produced-but-uncased action must be flagged as unhandled");

  WriteFile(root / "src/workspace/Uses.cpp",
            uses + "void Fix(){ Dispatch(ActionId::Orphan); }\n" +
                "void FixB(ActionId id){ switch (id) {\n    case ActionId::NeverHandled:\n"
                "      return;\n  } }\n");
  Expect(CheckEveryActionIdIsReachable(root).violations.empty(),
         "a producer for one and a case label for the other clears both halves");
}

void RunRegisteredSettingsAreReadRuleFixtures() {
  TemporaryDirectory settings_dir;
  const std::filesystem::path& root = settings_dir.path();
  std::filesystem::create_directories(root / "src/workspace");

  std::string registry = "std::span<const SettingSpec> BuiltinSettingSpecs() {\n";
  for (int i = 0; i < 24; ++i) {
    registry += "  SettingSpec{ .id = \"group" + std::to_string(i) + ".used_key\" },\n";
  }
  registry += "  SettingSpec{ .id = \"group.orphan_key\" },\n};\n";
  WriteFile(root / "src/workspace/registries/WorkspaceSettingsRegistry.cpp", registry);

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

  WriteFile(root / "src/workspace/Reader.cpp",
            reader + "bool Orphan(){ return SettingFlagEnabled(cfg, \"group.orphan_key\"); }\n");
  Expect(CheckRegisteredSettingsAreRead(root).violations.empty(),
         "a setting with any consumer passes");

  WriteFile(root / "src/workspace/registries/WorkspaceSettingsRegistry.cpp",
            "std::span<const SettingSpec> BuiltinSettingSpecs() { return {}; }\n");
  Expect(!CheckRegisteredSettingsAreRead(root).violations.empty(),
         "a registry the scan cannot parse must fail loudly, not pass vacuously");
}

void RunCoordinatorOperationsAreCalledRuleFixtures() {
  TemporaryDirectory ops_dir;
  const std::filesystem::path& root = ops_dir.path();
  std::filesystem::create_directories(root / "src/workspace");

  for (int i = 0; i < 60; ++i) {
    std::string padded = "#pragma once\nclass Pad" + std::to_string(i) +
                         " {\n public:\n  struct Operations {\n";
    for (int f = 0; f < 9; ++f) {
      padded += "    std::function<void()> pad" + std::to_string(i) + "_" + std::to_string(f) +
                ";\n";
    }
    padded += "  };\n};\n";
    WriteFile(root / ("src/workspace/Pad" + std::to_string(i) + ".h"), padded);
    std::string padded_use = "#include \"workspace/Pad" + std::to_string(i) + ".h\"\nvoid U" +
                             std::to_string(i) + "(Pad" + std::to_string(i) +
                             "::Operations& operations_) {\n";
    for (int f = 0; f < 9; ++f) {
      padded_use += "  operations_.pad" + std::to_string(i) + "_" + std::to_string(f) + "();\n";
    }
    padded_use += "}\n";
    WriteFile(root / ("src/workspace/Pad" + std::to_string(i) + ".cpp"), padded_use);
  }

  WriteFile(root / "src/workspace/Alpha.h",
            "#pragma once\n"
            "class Alpha {\n public:\n  struct Operations {\n"
            "    std::function<void()> called_hook;\n"
            "    std::function<void()> dead_hook;\n"
            "    std::function<void()> shared_name;\n"
            "  };\n};\n");
  WriteFile(root / "src/workspace/Alpha.cpp",
            "#include \"workspace/Alpha.h\"\n"
            "void Alpha::Run() {\n"
            "  operations_.called_hook();\n"
            "}\n"
            "Alpha MakeAlpha() {\n"
            "  return Alpha(Alpha::Operations{\n"
            "      .called_hook = []() {},\n"
            "      .dead_hook = []() {},\n"
            "      .shared_name = []() {},\n"
            "  });\n"
            "}\n");

  WriteFile(root / "src/workspace/Beta.h",
            "#pragma once\n"
            "class Beta {\n public:\n  struct Operations {\n"
            "    std::function<void()> shared_name;\n"
            "  };\n};\n");
  WriteFile(root / "src/workspace/Beta.cpp",
            "#include \"workspace/Beta.h\"\n"
            "void Beta::Run() { operations_.shared_name(); }\n");

  const RuleResult flagged = CheckCoordinatorOperationsAreCalled(root);
  Expect(flagged.violations.size() == 2,
         "exactly the never-called hooks must be flagged");
  const bool saw_dead =
      flagged.violations[0].message.find("dead_hook") != std::string::npos ||
      flagged.violations[1].message.find("dead_hook") != std::string::npos;
  const bool saw_shared =
      flagged.violations[0].message.find("shared_name") != std::string::npos ||
      flagged.violations[1].message.find("shared_name") != std::string::npos;
  Expect(saw_dead, "the plainly uncalled hook must be flagged");
  Expect(saw_shared,
         "a hook whose name is called only on an unrelated struct must still be flagged; reads "
         "are scoped by include graph, not by name");

  WriteFile(root / "src/workspace/AlphaExtra.cpp",
            "#include \"workspace/Alpha.h\"\n"
            "void Extra(Alpha::Operations& operations_) {\n"
            "  operations_.dead_hook();\n"
            "  operations_.shared_name();\n"
            "}\n");
  Expect(CheckCoordinatorOperationsAreCalled(root).violations.empty(),
         "a hook called from anything that includes its header passes");

  WriteFile(root / "src/workspace/AlphaExtra.cpp",
            "#include \"workspace/Alpha.h\"\n"
            "void Extra(Alpha::Operations& operations_) {\n"
            "  if (operations_.dead_hook) {\n  }\n"
            "  if (operations_.shared_name != nullptr) {\n  }\n"
            "}\n");
  Expect(CheckCoordinatorOperationsAreCalled(root).violations.empty(),
         "testing a hook for null counts as a read");

  TemporaryDirectory empty_dir;
  std::filesystem::create_directories(empty_dir.path() / "src/workspace");
  Expect(!CheckCoordinatorOperationsAreCalled(empty_dir.path()).violations.empty(),
         "a tree the scan cannot parse must fail loudly, not pass vacuously");
}

void RunRenderTuTextCompositionRuleFixtures() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path();
  std::filesystem::create_directories(root / "src/workspace");
  std::filesystem::create_directories(root / "src/editor");
  for (const char* name : {"WorkspaceShellRenderSidebar.cpp", "WorkspaceShellRenderCompare.cpp",
                           "WorkspaceShellRenderMerge.cpp", "WorkspaceShellHoverPopup.cpp",
                           "WorkspaceShellHoverTargets.cpp", "DebugPaneRender.cpp"}) {
    WriteFile(root / "src/workspace/render" / name, "// clean render TU fixture\n");
  }
  WriteFile(root / "src/editor/EditorViewRenderer.cpp", "// clean render TU fixture\n");
  WriteFile(root / "src/editor/DecoratedTextGridRenderer.cpp", "// clean render TU fixture\n");

  const std::filesystem::path sidebar = root / "src/workspace/render/WorkspaceShellRenderSidebar.cpp";

  WriteFile(sidebar, "void F(){ Draw(FormatEmptyState(\"matches\")); }\n");
  Expect(!CheckRenderTuDoesNotCallToStringOrFormat(root).violations.empty(),
         "the render-TU text rule must flag a WorkspaceUiText composer call");
  WriteFile(sidebar, "void F(){ Draw(JoinHintSegments({\"a\", \"b\"})); }\n");
  Expect(!CheckRenderTuDoesNotCallToStringOrFormat(root).violations.empty(),
         "the render-TU text rule must flag JoinHintSegments too, not just one composer");

  WriteFile(sidebar, "void F(std::string_view empty_text){ Draw(empty_text); }\n");
  const RuleResult clean = CheckRenderTuDoesNotCallToStringOrFormat(root);
  Expect(clean.violations.empty() && clean.missing_targets.empty(),
         "the render-TU text rule must pass when composed text arrives as a view");

  WriteFile(sidebar, "void F(const std::string& e){ auto s = \"Error: \" + e; (void)s; }\n");
  Expect(!CheckRenderTuDoesNotMaterializeSingleCharOrPrefixStrings(root).violations.empty(),
         "literal+ident concat in a surface render TU must be flagged");
  WriteFile(sidebar, "void F(std::string_view e){ Draw(e); }\n");
  Expect(CheckRenderTuDoesNotMaterializeSingleCharOrPrefixStrings(root).violations.empty(),
         "a surface render TU drawing a precomposed view must pass");

  WriteFile(root / "src/workspace/render/WorkspaceShellHoverPopup.cpp",
            "void F(const std::string& a, const std::string& b){ auto s = a + \" \" + b; (void)s; }\n");
  Expect(!CheckRenderTuDoesNotMaterializeSingleCharOrPrefixStrings(root).violations.empty(),
         "the hover popup is no longer exempt from the concat rule");
  WriteFile(root / "src/workspace/render/WorkspaceShellHoverPopup.cpp", "// clean render TU fixture\n");

  WriteFile(sidebar, "void F(){ Draw(text_renderer_.TruncateToWidth(label, w)); }\n");
  Expect(!CheckRenderTuDoesNotMaterializeSingleCharOrPrefixStrings(root).violations.empty(),
         "the owning TruncateToWidth must be flagged in a render TU");
  WriteFile(sidebar,
            "void F(){ Draw(text_renderer_.TruncateToWidthEphemeralView(label, w));"
            " Draw(TruncateLabelView(other, w)); }\n");
  const RuleResult truncation_ok = CheckRenderTuDoesNotMaterializeSingleCharOrPrefixStrings(root);
  Expect(truncation_ok.violations.empty() && truncation_ok.missing_targets.empty(),
         "the allocation-free truncation helpers must pass the render-TU rule");
}

void RunPaintedScrollbarRuleFixtures() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path();
  std::filesystem::create_directories(root / "src/workspace");
  const std::filesystem::path pane = root / "src/workspace/SomePaneRender.cpp";

  WriteFile(pane, "void F(){ DrawScrollbar(r, t, bar->track, bar->thumb, false); }\n");
  Expect(!CheckPaintedScrollbarsAreGrabbable(root).violations.empty(),
         "a scrollbar painted with a literal false active flag must be flagged");

  WriteFile(pane,
            "void F(){ DrawScrollbar(r, t, bar->track, bar->thumb,\n"
            "    state.drag_target == DragTarget::SomeScrollbar); }\n");
  const RuleResult clean = CheckPaintedScrollbarsAreGrabbable(root);
  Expect(clean.violations.empty() && clean.missing_targets.empty(),
         "a scrollbar whose active flag is a live drag-target comparison must pass");

  WriteFile(pane,
            "void F(bool active){ DrawScrollbar(r, t, MakeBar(x, false), bar->thumb, active); }\n");
  Expect(CheckPaintedScrollbarsAreGrabbable(root).violations.empty(),
         "a threaded active flag must pass even when the call has another false argument");

  TemporaryDirectory empty_dir;
  std::filesystem::create_directories(empty_dir.path() / "src/workspace");
  Expect(!CheckPaintedScrollbarsAreGrabbable(empty_dir.path()).violations.empty(),
         "a scan with no translation units must report a violation, not pass vacuously");
}

void RunMissingRuleTargetFixtures() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path();

  const RuleResult absent = CheckEditorViewModelStickyAndOccurrenceAreSpans(root);
  Expect(!absent.missing_targets.empty(),
         "a rule whose target file is absent must report the missing target, not pass silently");
  Expect(absent.violations.empty(),
         "a missing target is a broken rule, not a code violation -- the two must stay separate");

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

void RunPerfCounterProducerRuleFixtures() {
  TemporaryDirectory counter_dir;
  const std::filesystem::path& root = counter_dir.path();
  std::filesystem::create_directories(root / "src/util");
  std::filesystem::create_directories(root / "src/workspace");

  const char* kHeader =
      "#define MICROIDE_PERF_COUNTERS(X) \\\n"
      "  X(WiredUp, \"a.wired_up\") \\\n"
      "  X(Orphan, \"a.orphan\")\n";

  WriteFile(root / "src/util/PerformanceCounters.h", kHeader);
  WriteFile(root / "src/workspace/User.cpp",
            "void f(){ AddPerformanceCounter(util::PerfCounterId::WiredUp); }\n");
  const RuleResult orphaned = CheckEveryPerfCounterHasAProducer(root);
  Expect(orphaned.violations.size() == 1,
         "perf-counter rule must flag a declared counter that nothing increments");

  WriteFile(root / "src/workspace/User.cpp",
            "void f(){ AddPerformanceCounter(util::PerfCounterId::WiredUp); }\n"
            "void g(){ AddPerformanceCounter(util::PerfCounterId::Orphan, 2); }\n");
  Expect(CheckEveryPerfCounterHasAProducer(root).violations.empty(),
         "perf-counter rule must accept a counter that is incremented anywhere in src/");

  WriteFile(root / "src/util/PerformanceCounters.cpp",
            "const char* kNames[] = { \"a.wired_up\", \"a.orphan\" };\n"
            "void h(){ (void)PerfCounterId::Orphan; }\n");
  WriteFile(root / "src/workspace/User.cpp",
            "void f(){ AddPerformanceCounter(util::PerfCounterId::WiredUp); }\n");
  Expect(CheckEveryPerfCounterHasAProducer(root).violations.size() == 1,
         "perf-counter rule must not count the declaration or the name table as a producer");

  WriteFile(root / "src/util/PerformanceCounters.h",
            "#define MICROIDE_PERF_COUNTERS(X)\nenum class PerfCounterId { Count };\n");
  Expect(!CheckEveryPerfCounterHasAProducer(root).violations.empty(),
         "perf-counter rule must fail loudly when it finds no counter declarations");
}

void RunEditorGroupsSplitTreeRuleFixtures() {
  TemporaryDirectory groups_dir;
  const std::filesystem::path& root = groups_dir.path();
  std::filesystem::create_directories(root / "src/workspace/coordinators");

  WriteFile(root / "src/workspace/coordinators/Groups.cpp",
            "void Collapse(std::size_t gi) {\n"
            "  state_.editor_groups.erase(state_.editor_groups.begin() + gi);\n"
            "}\n");
  Expect(CheckEditorGroupsMutateWithTheSplitTree(root).violations.size() == 1,
         "editor-groups rule must flag a group erase in a TU that never names editor_split");

  WriteFile(root / "src/workspace/coordinators/Groups.cpp",
            "void Collapse(std::size_t gi) {\n"
            "  state_.editor_groups.erase(state_.editor_groups.begin() + gi);\n"
            "  state_.editor_split.RemoveLeaf(gi);\n"
            "}\n");
  Expect(CheckEditorGroupsMutateWithTheSplitTree(root).violations.empty(),
         "editor-groups rule must accept a mutation whose TU moves the split tree too");

  WriteFile(root / "src/workspace/coordinators/Groups.cpp",
            "// editor_groups.erase(it) used to live here.\n"
            "const char* kDoc = \"editor_groups.push_back(group)\";\n"
            "std::size_t Count() { return state_.editor_groups.size(); }\n");
  const RuleResult commented = CheckEditorGroupsMutateWithTheSplitTree(root);
  Expect(commented.violations.size() == 1 &&
             commented.violations.front().message.find("vacuous") != std::string::npos,
         "editor-groups rule must not match comments or string literals, and must say so loudly "
         "when that leaves it scanning nothing");

  WriteFile(root / "src/workspace/coordinators/Groups.cpp",
            "void Collapse(std::size_t gi) {\n"
            "  const auto rects = ComputeEditorGroupRects(layout, state_.editor_split);\n"
            "  state_.editor_groups.erase(state_.editor_groups.begin() + gi);\n"
            "}\n");
  Expect(CheckEditorGroupsMutateWithTheSplitTree(root).violations.size() == 1,
         "editor-groups rule must flag a TU that only reads the tree it fails to update");

  WriteFile(root / "src/workspace/coordinators/Reader.cpp",
            "auto& Group(std::size_t gi) { return state_.editor_groups[gi]; }\n"
            "std::size_t Count() { return state_.editor_groups.size(); }\n");
  WriteFile(root / "src/workspace/coordinators/Groups.cpp",
            "void Add() {\n"
            "  state_.editor_groups.push_back(EditorGroup{});\n"
            "  state_.editor_split.InsertLeaf(0, orientation, false);\n"
            "}\n");
  Expect(CheckEditorGroupsMutateWithTheSplitTree(root).violations.empty(),
         "editor-groups rule must leave read-only group access alone");
}

void RunViewportFiletypeMemoRuleFixtures() {
  TemporaryDirectory filetype_dir;
  const std::filesystem::path& root = filetype_dir.path();
  std::filesystem::create_directories(root / "src/editor");
  std::filesystem::create_directories(root / "src/workspace");
  WriteFile(root / "src/editor/TextViewport.cpp",
            "const std::string& TextViewport::language_id() const {\n"
            "  language_id_ = runtime_syntax::DetectFiletype(document_->path, lines);\n"
            "  return language_id_;\n"
            "}\n");

  WriteFile(root / "src/workspace/Caller.cpp",
            "void f(){ auto id = DetectFiletype(viewport.path(), viewport.lines()); }\n");
  Expect(CheckViewportFiletypeGoesThroughTheViewportMemo(root).violations.size() == 1,
         "viewport-filetype rule must flag DetectFiletype(path, lines) outside the memo");

  WriteFile(root / "src/workspace/Caller.cpp",
            "void f(){ auto id = DetectFiletype(bucket.path); }\n"
            "void g(){ auto id = DetectFiletype(Normalize(a, b)); }\n");
  Expect(CheckViewportFiletypeGoesThroughTheViewportMemo(root).violations.empty(),
         "viewport-filetype rule must accept the path-only overload, including a nested call");

  WriteFile(root / "src/workspace/Caller.cpp",
            "// DetectFiletype(path, lines) is what this used to do.\n"
            "const char* kDoc = \"DetectFiletype(path, lines)\";\n"
            "void f(){ auto id = DetectFiletype(bucket.path); }\n");
  Expect(CheckViewportFiletypeGoesThroughTheViewportMemo(root).violations.empty(),
         "viewport-filetype rule must not match comments or string literals");

  WriteFile(root / "src/editor/TextViewport.cpp", "void nothing() {}\n");
  Expect(!CheckViewportFiletypeGoesThroughTheViewportMemo(root).violations.empty(),
         "viewport-filetype rule must fail loudly when the memo no longer detects anything");
}

void RunPerfHarnessIsolationOrderRuleFixtures() {
  TemporaryDirectory harness_dir;
  const std::filesystem::path& root = harness_dir.path();
  std::filesystem::create_directories(root / "tests/perf");
  const auto harness = root / "tests/perf/PerfHarness.cpp";

  WriteFile(harness,
            "std::optional<Aggregate> PerfHarness::RunScenario() {\n"
            "  Driver driver;\n"
            "  driver.isolated_app_root = EstablishIsolatedAppRoot(false, &error);\n"
            "}\n");
  Expect(CheckPerfHarnessIsolatesBeforeConstructingTheShell(root).violations.size() == 1,
         "perf-isolation-order rule must flag isolation established after `Driver driver;`");

  WriteFile(harness,
            "std::optional<Aggregate> PerfHarness::RunScenario() {\n"
            "  const auto root = EstablishIsolatedAppRoot(false, &error);\n"
            "  Driver driver;\n"
            "  driver.isolated_app_root = root;\n"
            "}\n");
  Expect(CheckPerfHarnessIsolatesBeforeConstructingTheShell(root).violations.empty(),
         "perf-isolation-order rule must accept isolation established before the Driver");

  WriteFile(harness,
            "std::optional<Aggregate> PerfHarness::RunScenario() {\n"
            "  // EstablishIsolatedAppRoot used to run here.\n"
            "  Driver driver;\n"
            "}\n");
  Expect(!CheckPerfHarnessIsolatesBeforeConstructingTheShell(root).violations.empty(),
         "perf-isolation-order rule must not accept a commented-out anchor as the real one");

  WriteFile(harness, "void nothing() {}\n");
  Expect(!CheckPerfHarnessIsolatesBeforeConstructingTheShell(root).violations.empty(),
         "perf-isolation-order rule must fail loudly when its anchors are gone");
}

void RunPerfMeasureBodyRuleFixtures() {
  TemporaryDirectory scenarios_dir;
  const std::filesystem::path& root = scenarios_dir.path();
  std::filesystem::create_directories(root / "tests/perf");
  const auto scenario = root / "tests/perf/ExampleScenarios.cpp";

  WriteFile(scenario,
            "void RunThing(ScenarioContext& context) {\n"
            "  context.Measure(\"thing.apply\", [&]() {\n"
            "    for (int i = 0; i < 40; ++i) {\n"
            "      Apply(\"item_\" + std::to_string(i));\n"
            "    }\n"
            "  });\n"
            "}\n");
  Expect(CheckPerfMeasureBodiesDoNotBuildTheirOwnInput(root).violations.size() == 1,
         "perf-measure rule must flag string composition inside a Measure body");

  WriteFile(scenario,
            "void RunThing(ScenarioContext& context) {\n"
            "  std::vector<std::string> input;\n"
            "  for (int i = 0; i < 40; ++i) input.push_back(\"item_\" + std::to_string(i));\n"
            "  context.Measure(\"thing.apply\", [&]() {\n"
            "    for (const std::string& id : input) {\n"
            "      Apply(id);\n"
            "    }\n"
            "  });\n"
            "}\n");
  Expect(CheckPerfMeasureBodiesDoNotBuildTheirOwnInput(root).violations.empty(),
         "perf-measure rule must accept composition done OUTSIDE the measured window");

  WriteFile(scenario,
            "void RunThing(ScenarioContext& context) {\n"
            "  context.Measure(\"config.encode\", [&]() {\n"
            "    // perf-measure-builds-input: encoding a record IS the measured work.\n"
            "    for (int i = 0; i < 40; ++i) {\n"
            "      sink += \"key=\" + std::to_string(i);\n"
            "    }\n"
            "  });\n"
            "}\n");
  Expect(CheckPerfMeasureBodiesDoNotBuildTheirOwnInput(root).violations.empty(),
         "perf-measure rule must honor a declared perf-measure-builds-input exemption");

  WriteFile(scenario, "void RunThing(ScenarioContext& context) { context.PumpFrames(2); }\n");
  Expect(!CheckPerfMeasureBodiesDoNotBuildTheirOwnInput(root).violations.empty(),
         "perf-measure rule must fail loudly when it finds no Measure body to scan");
}

void RunPerfMeasureWallClockWaitRuleFixtures() {
  TemporaryDirectory scenarios_dir;
  const std::filesystem::path& root = scenarios_dir.path();
  std::filesystem::create_directories(root / "tests/perf");
  const auto scenario = root / "tests/perf/ExampleScenarios.cpp";

  WriteFile(scenario,
            "void RunThing(ScenarioContext& context) {\n"
            "  context.Measure(\"linter.wait_diagnostics\", [&]() {\n"
            "    (void)context.WaitForDiagnostics(source, std::chrono::milliseconds(120));\n"
            "  });\n"
            "}\n");
  Expect(CheckPerfMeasureBodiesDoNotWaitOnWallClock(root).violations.size() == 1,
         "perf-wait rule must flag a wall-clock wait inside a Measure body");

  WriteFile(scenario,
            "void RunThing(ScenarioContext& context) {\n"
            "  const bool ready = context.WaitForDiagnostics(source, kTimeout);\n"
            "  context.Measure(\"linter.apply\", [&]() {\n"
            "    context.PumpFrames(3);\n"
            "  });\n"
            "  if (!ready) context.SkipScenario(\"no diagnostics\");\n"
            "}\n");
  Expect(CheckPerfMeasureBodiesDoNotWaitOnWallClock(root).violations.empty(),
         "perf-wait rule must accept a wait done OUTSIDE the measured window");

  WriteFile(scenario,
            "void RunThing(ScenarioContext& context) {\n"
            "  context.Measure(\"repo_open.idle_500ms\", [&]() {\n"
            "    // perf-measure-waits-on-clock: the fixed idle IS the measurement.\n"
            "    (void)context.Wait(std::chrono::milliseconds(500));\n"
            "  });\n"
            "}\n");
  Expect(CheckPerfMeasureBodiesDoNotWaitOnWallClock(root).violations.empty(),
         "perf-wait rule must honor a declared perf-measure-waits-on-clock exemption");

  WriteFile(scenario, "void RunThing(ScenarioContext& context) { context.PumpFrames(2); }\n");
  Expect(!CheckPerfMeasureBodiesDoNotWaitOnWallClock(root).violations.empty(),
         "perf-wait rule must fail loudly when it finds no Measure body to scan");
}

void RunShellGlueBindingRuleFixtures() {
  TemporaryDirectory workspace_dir;
  const std::filesystem::path& root = workspace_dir.path();
  std::filesystem::create_directories(root / "src/workspace");
  const auto source = root / "src/workspace/ExampleGlue.cpp";

  WriteFile(source,
            "void Shell::Sweep() {\n"
            "  EditorTabService editor_tabs = MakeEditorTabService();\n"
            "  editor_tabs.Save(0);\n"
            "}\n");
  Expect(CheckShellGlueIsBoundByReference(root).violations.size() == 1,
         "the glue-binding rule must flag a value binding of a cached factory");

  WriteFile(source,
            "void Shell::Sweep() {\n"
            "  auto persistence = MakePersistenceCoordinator();\n"
            "  persistence.Flush();\n"
            "}\n");
  Expect(CheckShellGlueIsBoundByReference(root).violations.size() == 1,
         "`auto` without `&` is the same copy and must be flagged too");

  WriteFile(source,
            "void Shell::Sweep() {\n"
            "  auto& editor_tabs = MakeEditorTabService();\n"
            "  EditorTabService& again = MakeEditorTabService();\n"
            "  editor_tabs.Save(0);\n"
            "  again.Save(1);\n"
            "}\n");
  Expect(CheckShellGlueIsBoundByReference(root).violations.empty(),
         "binding by reference is the fix and must pass");

  WriteFile(source,
            "void Shell::Sweep() {\n"
            "  MakeEditorTabService().Save(0);\n"
            "  const bool ok = MakeSidebarMouseCoordinator().HandleDrag(e, l);\n"
            "}\n");
  Expect(CheckShellGlueIsBoundByReference(root).violations.empty(),
         "a discarded temporary is not a binding and must not be flagged");

  WriteFile(source, "void Shell::Sweep() {}\n");
  Expect(!CheckShellGlueIsBoundByReference(root).missing_targets.empty(),
         "a tree with no factory calls must report a missing target, not pass");
}

void RunFactoryCaptureRuleFixtures() {
  TemporaryDirectory workspace_dir;
  const std::filesystem::path& root = workspace_dir.path();
  std::filesystem::create_directories(root / "src/workspace");
  const auto source = root / "src/workspace/ExampleFactory.cpp";

  WriteFile(source,
            "Coord Shell::MakeCoord() {\n"
            "  auto terminal_panel = MakeTerminalPanelService();\n"
            "  return Coord(Ops{\n"
            "      .open_terminal = [terminal_panel](std::string c) mutable {\n"
            "        terminal_panel.OpenTerminal(std::move(c));\n"
            "      },\n"
            "  });\n"
            "}\n");
  Expect(CheckFactoryResultsAreNotCapturedByValue(root).violations.size() == 1,
         "factory-capture rule must flag a by-value capture of a Make*Service() local");

  WriteFile(source,
            "Coord Shell::MakeCoord() {\n"
            "  auto terminal_panel = MakeTerminalPanelService();\n"
            "  terminal_panel.Warm();\n"
            "  return Coord(Ops{\n"
            "      .open_terminal = [this](std::string c) {\n"
            "        MakeTerminalPanelService().OpenTerminal(std::move(c));\n"
            "      },\n"
            "  });\n"
            "}\n");
  Expect(CheckFactoryResultsAreNotCapturedByValue(root).violations.empty(),
         "factory-capture rule must accept constructing the service inside the body");

  WriteFile(source,
            "Coord Shell::MakeCoord() {\n"
            "  auto& terminal_panel = MakeTerminalPanelService();\n"
            "  return Coord(Ops{\n"
            "      .open_terminal = [terminal_panel](std::string c) mutable {\n"
            "        terminal_panel.OpenTerminal(std::move(c));\n"
            "      },\n"
            "  });\n"
            "}\n");
  Expect(CheckFactoryResultsAreNotCapturedByValue(root).violations.size() == 1,
         "a reference-bound factory local captured BY VALUE is still a per-hook copy");

  WriteFile(source,
            "Coord Shell::MakeCoord() {\n"
            "  auto& terminal_panel = MakeTerminalPanelService();\n"
            "  return Coord(Ops{\n"
            "      .open_terminal = [&terminal_panel](std::string c) {\n"
            "        terminal_panel.OpenTerminal(std::move(c));\n"
            "      },\n"
            "  });\n"
            "}\n");
  Expect(CheckFactoryResultsAreNotCapturedByValue(root).violations.empty(),
         "a reference capture of a reference-bound factory local is free");

  WriteFile(source,
            "Coord Shell::MakeCoord() {\n"
            "  auto terminal_panel = MakeTerminalPanelService();\n"
            "  // note: [terminal_panel] by value would be a copy per hook\n"
            "  const auto run = [&terminal_panel]() { terminal_panel.Warm(); };\n"
            "  run();\n"
            "  return Coord(Ops{});\n"
            "}\n");
  Expect(CheckFactoryResultsAreNotCapturedByValue(root).violations.empty(),
         "factory-capture rule must not flag a reference capture or a comment mention");

  WriteFile(source, "Coord Shell::MakeCoord() { return Coord(Ops{}); }\n");
  Expect(!CheckFactoryResultsAreNotCapturedByValue(root).violations.empty(),
         "factory-capture rule must fail loudly when it finds no factory local to scan");
}

void RunKernelWindowingLibraryRuleFixtures() {
  TemporaryDirectory kernel_dir;
  const std::filesystem::path& root = kernel_dir.path();
  std::filesystem::create_directories(root / "src/util");
  std::filesystem::create_directories(root / "src/terminal");
  std::filesystem::create_directories(root / "src/render");
  const auto write_cmake = [&](std::string_view sources) {
    WriteFile(root / "CMakeLists.txt",
              std::string("set(MICROIDE_KERNEL_SOURCES\n") + std::string(sources) + ")\n");
  };
  write_cmake("  src/terminal/Leaf.cpp\n");

  WriteFile(root / "src/util/Direct.h", "struct S { Uint32 wake = 0; };\n");
  WriteFile(root / "src/render/Paint.h", "#include <SDL3/SDL.h>\nstruct Paint {};\n");
  WriteFile(root / "src/util/Middle.h", "#include \"render/Paint.h\"\nstruct Middle {};\n");
  WriteFile(root / "src/terminal/Leaf.cpp", "#include \"util/Middle.h\"\nvoid F(){}\n");
  const RuleResult flagged = CheckKernelStaysFreeOfTheWindowingLibrary(root);
  Expect(flagged.violations.size() == 3,
         "kernel rule must flag the direct SDL token, the intermediate header, and the "
         "kernel TU that only reaches SDL transitively");

  WriteFile(root / "src/util/Direct.h", "#include <cstdint>\nstruct S { std::uint32_t wake = 0; };\n");
  WriteFile(root / "src/util/Middle.h", "#include \"util/Direct.h\"\nstruct Middle {};\n");
  Expect(CheckKernelStaysFreeOfTheWindowingLibrary(root).violations.empty(),
         "kernel rule must accept kernel files that name only plain types and include "
         "only clean kernel headers");

  WriteFile(root / "src/render/Uses.cpp", "#include \"render/Paint.h\"\nvoid G(){}\n");
  Expect(CheckKernelStaysFreeOfTheWindowingLibrary(root).violations.empty(),
         "kernel rule must not flag shell-layer files for using the windowing library");

  std::filesystem::create_directories(root / "src/editor");
  WriteFile(root / "src/editor/Plain.cpp", "#include \"render/Paint.h\"\nvoid H(){}\n");
  Expect(CheckKernelStaysFreeOfTheWindowingLibrary(root).violations.empty(),
         "a file outside the kernel directories is not kernel until the build says so");
  write_cmake("  src/terminal/Leaf.cpp\n  src/editor/Plain.cpp\n");
  Expect(CheckKernelStaysFreeOfTheWindowingLibrary(root).violations.size() == 1,
         "naming a file in MICROIDE_KERNEL_SOURCES brings it under the rule wherever it lives");
  write_cmake("  src/terminal/Leaf.cpp\n");

  WriteFile(root / "src/terminal/Leaf.cpp", "#include \"util/Typod.h\"\nvoid F(){}\n");
  Expect(!CheckKernelStaysFreeOfTheWindowingLibrary(root).missing_targets.empty(),
         "kernel rule must report an include it cannot resolve rather than assume it clean");

  WriteFile(root / "src/terminal/Leaf.cpp", "void F(){}\n");
  WriteFile(root / "CMakeLists.txt", "set(MICROIDE_SOMETHING_ELSE)\n");
  Expect(!CheckKernelStaysFreeOfTheWindowingLibrary(root).missing_targets.empty(),
         "kernel rule must report a MICROIDE_KERNEL_SOURCES list it cannot read");
}

void RunGitLayerLocalityRuleFixtures() {
  TemporaryDirectory dir;
  const std::filesystem::path& root = dir.path();
  std::filesystem::create_directories(root / "src/project");

  WriteFile(root / "src/project/GitThing.cpp",
            "void F(const std::filesystem::path& root){\n"
            "  const GitRepository repo(root, platform::LocalProcessLauncher());\n}\n");
  const RuleResult flagged = CheckGitLayerDoesNotChooseLocality(root);
  Expect(flagged.violations.size() == 1,
         "git-locality rule must flag a git unit that constructs LocalProcessLauncher()");
  Expect(flagged.missing_targets.empty(),
         "and a GitRepository built from an argument still counts as something to scan");

  WriteFile(root / "src/project/GitThing.cpp",
            "void F(const std::filesystem::path& root, const platform::ProcessLauncher& l){\n"
            "  const GitRepository repo(root, l);\n}\n");
  Expect(CheckGitLayerDoesNotChooseLocality(root).violations.empty(),
         "git-locality rule must accept a launcher passed in by the caller");

  WriteFile(root / "src/project/Other.cpp", "void G(){ platform::LocalProcessLauncher(); }\n");
  WriteFile(root / "src/project/CommitNotes.cpp",
            "// used to call LocalProcessLauncher() here\n"
            "void H(const std::filesystem::path& r, const L& l){ GitRepository repo(r, l); }\n");
  Expect(CheckGitLayerDoesNotChooseLocality(root).violations.empty(),
         "git-locality rule must ignore non-git units and comments");

  std::filesystem::remove(root / "src/project/GitThing.cpp");
  std::filesystem::remove(root / "src/project/CommitNotes.cpp");
  Expect(!CheckGitLayerDoesNotChooseLocality(root).missing_targets.empty(),
         "git-locality rule must report a scope with no GitRepository construction");
}

void RunGitMetadataRuleFixtures() {
  TemporaryDirectory dir;
  const std::filesystem::path& root = dir.path();
  std::filesystem::create_directories(root / "src/project");
  std::filesystem::create_directories(root / "src/workspace");
  WriteFile(root / "src/project/GitMetadataSource.cpp",
            "bool A(const P& r){ return internal::HasGitMarker(r); }\n");

  WriteFile(root / "src/workspace/Probe.cpp",
            "bool F(const P& r){ return project::internal::HasGitMarker(r); }\n");
  WriteFile(root / "src/project/GitThing.cpp",
            "auto G(const P& r){ return internal::ResolveGitDirectory(r); }\n");
  const RuleResult flagged = CheckGitMetadataIsAskedOfTheHost(root);
  Expect(flagged.violations.size() == 2,
         "git-metadata rule must flag a local .git probe outside the metadata source");
  Expect(flagged.missing_targets.empty(), "and the local source still counts as its target");

  WriteFile(root / "src/workspace/Probe.cpp",
            "// used to call HasGitMarker(root) here\n"
            "bool F(const P& r, const L& l){ return GitMetadataFor(l).IsRepository(r); }\n");
  WriteFile(root / "src/project/GitThing.cpp",
            "auto G(const P& r, const L& l){ return GitMetadataFor(l).ReadableGitDirectory(r); }\n");
  Expect(CheckGitMetadataIsAskedOfTheHost(root).violations.empty(),
         "git-metadata rule must accept probes routed through GitMetadataFor and comments");

  WriteFile(root / "src/project/GitMetadataSource.cpp", "bool A(){ return true; }\n");
  Expect(!CheckGitMetadataIsAskedOfTheHost(root).missing_targets.empty(),
         "git-metadata rule must report a local source that no longer probes");
}

void RunProcessLauncherRuleFixtures() {
  TemporaryDirectory launcher_dir;
  const std::filesystem::path& root = launcher_dir.path();
  std::filesystem::create_directories(root / "src/platform");
  std::filesystem::create_directories(root / "src/workspace");
  WriteFile(root / "src/platform/ProcessLauncher.cpp",
            "SubprocessResult Run(){ return RunSubprocess(argv, options); }\n");

  WriteFile(root / "src/workspace/BadSync.cpp",
            "void F(){ platform::RunSubprocess({\"git\"}, {}); }\n");
  WriteFile(root / "src/workspace/BadAsync.cpp",
            "platform::AsyncSubprocess proc;\nvoid G(){ proc.Start(command, cwd, sandbox); }\n");
  const RuleResult flagged = CheckEverySpawnGoesThroughAProcessLauncher(root);
  Expect(flagged.violations.size() == 2,
         "spawn rule must flag both a direct RunSubprocess and an AsyncSubprocess start "
         "with no launcher-resolved argv");

  WriteFile(root / "src/workspace/BadSync.cpp",
            "void F(){ platform::LocalProcessLauncher().Run({\"git\"}, {}); }\n");
  WriteFile(root / "src/workspace/BadAsync.cpp",
            "platform::AsyncSubprocess proc;\n"
            "void G(){ proc.Start(launcher.ResolveArgv(command), cwd, sandbox); }\n");
  Expect(CheckEverySpawnGoesThroughAProcessLauncher(root).violations.empty(),
         "spawn rule must accept spawns routed through a ProcessLauncher");

  WriteFile(root / "src/workspace/Holder.h", "struct T { platform::AsyncSubprocess proc; };\n");
  Expect(CheckEverySpawnGoesThroughAProcessLauncher(root).violations.empty(),
         "spawn rule must not flag a file that only holds an AsyncSubprocess");

  WriteFile(root / "src/platform/ProcessLauncher.cpp", "SubprocessResult Run(){ return {}; }\n");
  Expect(!CheckEverySpawnGoesThroughAProcessLauncher(root).missing_targets.empty(),
         "spawn rule must report that the launcher no longer calls the spawn primitive");

  WriteFile(root / "src/platform/Subprocess.h",
            "SubprocessResult RunSubprocess(const std::vector<std::string>& argv,\n"
            "                              const SubprocessOptions& options = {});\n");
  Expect(!CheckEverySpawnGoesThroughAProcessLauncher(root).missing_targets.empty(),
         "spawn rule's vacuity guard must not be satisfied by the primitive's "
         "declaration in an exempt header");

  WriteFile(root / "src/platform/ProcessLauncher.cpp",
            "// Run() used to call RunSubprocess(argv, options) directly.\n"
            "SubprocessResult Run(){ return {}; }\n");
  Expect(!CheckEverySpawnGoesThroughAProcessLauncher(root).missing_targets.empty(),
         "spawn rule's vacuity guard must not be satisfied by a comment");

  WriteFile(root / "src/platform/ProcessLauncher.cpp",
            "SubprocessResult Run(){ return RunSubprocess(argv, options); }\n");
  Expect(CheckEverySpawnGoesThroughAProcessLauncher(root).missing_targets.empty(),
         "spawn rule must accept a launcher that still calls the spawn primitive");
}

void RunWriteGateRuleFixtures() {
  TemporaryDirectory gate_dir;
  const std::filesystem::path& root = gate_dir.path();
  std::filesystem::create_directories(root / "src/project");
  std::filesystem::create_directories(root / "src/workspace");
  std::filesystem::create_directories(root / "src/editor");
  std::filesystem::create_directories(root / "src/platform");
  WriteFile(root / "src/project/FileWriteGate.cpp",
            "bool W(){ return util::WriteTextFileAtomically(p, t); }\n");
  WriteFile(root / "src/project/LocalTreeOps.cpp",
            "bool M(){ return platform::MovePathNoOverwrite(a, b); }\n");

  WriteFile(root / "src/workspace/Replace.cpp",
            "void F(){ util::WriteTextFileAtomically(path, text); }\n");
  WriteFile(root / "src/editor/Save.cpp",
            "void G(){ util::WriteTextFileAtomically(path, text); }\n");
  WriteFile(root / "src/plugin/Stream.cpp",
            "void H(){ std::ofstream out(path); out << text; }\n");
  const RuleResult flagged = CheckProjectWritesGoThroughTheWriteGate(root);
  Expect(flagged.violations.size() == 3,
         "the write-gate rule must flag a raw atomic write AND a raw ofstream in the "
         "layers that hold project files open");

  WriteFile(root / "src/workspace/Replace.cpp",
            "void F(){ project::LocalFileWriteGate().WriteText(path, text); }\n");
  WriteFile(root / "src/editor/Save.cpp",
            "void G(){ write_gate_->WriteText(path, text); }\n");
  WriteFile(root / "src/plugin/Stream.cpp",
            "void H(){ project::LocalFileWriteGate().WriteText(path, text); }\n");
  Expect(CheckProjectWritesGoThroughTheWriteGate(root).violations.empty(),
         "the write-gate rule must accept writes routed through the gate");

  std::filesystem::create_directories(root / "src/workspace/lsp");
  std::filesystem::create_directories(root / "src/workspace/control");
  WriteFile(root / "src/workspace/control/ControlChannelService.cpp",
            "void K(){ std::ofstream out(temp_path); }\n");
  Expect(CheckProjectWritesGoThroughTheWriteGate(root).violations.empty(),
         "the control descriptor is a named exemption");
  WriteFile(root / "src/workspace/lsp/LspService.cpp",
            "void J(){ std::ofstream created(target); }\n");
  Expect(CheckProjectWritesGoThroughTheWriteGate(root).violations.size() == 1,
         "the LSP service is no longer exempt");
  std::filesystem::remove(root / "src/workspace/lsp/LspService.cpp");
  WriteFile(root / "src/workspace/lsp/Other.cpp",
            "void L(){ std::ofstream created(target); }\n");
  Expect(CheckProjectWritesGoThroughTheWriteGate(root).violations.size() == 1,
         "the exemption is per FILE, not per directory");
  std::filesystem::remove(root / "src/workspace/lsp/Other.cpp");

  WriteFile(root / "src/platform/FsOps.cpp",
            "void H(){ util::WriteTextFileAtomically(path, text); }\n");
  Expect(CheckProjectWritesGoThroughTheWriteGate(root).violations.empty(),
         "the write-gate rule is scoped to the layers that hold project files open");

  std::filesystem::create_directories(root / "src/workspace/coordinators");
  WriteFile(root / "src/workspace/coordinators/Sidebar.cpp",
            "void T(){ platform::MovePathToTrash(p); }\n");
  WriteFile(root / "src/workspace/lsp/Ops.cpp",
            "void R(){ std::error_code ec; fs::rename(a, b, ec); }\n");
  WriteFile(root / "src/workspace/control/Descriptor.cpp",
            "void D(){ std::error_code ec; std::filesystem::remove(p, ec); }\n");
  Expect(CheckProjectWritesGoThroughTheWriteGate(root).violations.size() == 2,
         "the tree half flags a platform primitive and a raw fs mutation where the "
         "project tree is acted on, and not a descriptor cleanup elsewhere");
  WriteFile(root / "src/workspace/coordinators/Sidebar.cpp",
            "void T(){ (void)gate.ApplyTreeOp(op); }\n");
  WriteFile(root / "src/workspace/lsp/Ops.cpp",
            "// used to call fs::rename(a, b, ec) here\nvoid R(){ (void)gate.ApplyTreeOps(ops); }\n");
  Expect(CheckProjectWritesGoThroughTheWriteGate(root).violations.empty(),
         "and accepts the gate, and a mention in a comment");
  WriteFile(root / "src/project/LocalTreeOps.cpp", "bool M(){ return false; }\n");
  Expect(!CheckProjectWritesGoThroughTheWriteGate(root).missing_targets.empty(),
         "the tree half reports local tree ops that no longer call a move primitive");
  WriteFile(root / "src/project/LocalTreeOps.cpp",
            "bool M(){ return platform::MovePathNoOverwrite(a, b); }\n");

  WriteFile(root / "src/project/FileWriteGate.cpp", "bool W(){ return false; }\n");
  Expect(!CheckProjectWritesGoThroughTheWriteGate(root).missing_targets.empty(),
         "the write-gate rule must report that the gate no longer calls the primitive");
}

void RunWorkerDeclarationOrderRuleFixtures() {
  TemporaryDirectory worker_dir;
  const std::filesystem::path& root = worker_dir.path();
  std::filesystem::create_directories(root / "src/workspace");
  const auto header = root / "src/workspace/ExampleService.h";

  WriteFile(header,
            "class ExampleService {\n"
            " private:\n"
            "  util::TaskExecutor executor_{1};\n"
            "  util::MainThreadMailbox mailbox_;\n"
            "};\n");
  Expect(CheckWorkersOutliveNothingTheyWrite(root).violations.size() == 1,
         "an executor declared before its mailbox must be flagged");

  WriteFile(header,
            "class ExampleService {\n"
            " private:\n"
            "  util::MainThreadMailbox mailbox_;\n"
            "  util::TaskExecutor executor_{1};\n"
            "};\n");
  Expect(CheckWorkersOutliveNothingTheyWrite(root).violations.empty(),
         "an executor declared last must pass");
  Expect(CheckWorkersOutliveNothingTheyWrite(root).missing_targets.empty(),
         "and a tree with the pair is not a missing target");

  WriteFile(header, "class ExampleService {};\n");
  Expect(!CheckWorkersOutliveNothingTheyWrite(root).missing_targets.empty(),
         "a tree with no executor/mailbox pair must report a missing target");
}

void RunTextViewportDefaultedSpecialMembersRuleFixtures() {
  TemporaryDirectory dir;
  const std::filesystem::path& root = dir.path();
  std::filesystem::create_directories(root / "src/editor");
  const std::string defaulted_header =
      "class TextViewport {\n"
      " public:\n"
      "  TextViewport(const TextViewport& other) = default;\n"
      "  TextViewport& operator=(const TextViewport& other) = default;\n"
      "  TextViewport(TextViewport&& other) noexcept = default;\n"
      "  TextViewport& operator=(TextViewport&& other) noexcept = default;\n"
      "};\n";
  WriteFile(root / "src/editor/TextViewport.h", defaulted_header);
  WriteFile(root / "src/editor/TextViewport.cpp",
            "// TextViewport::TextViewport(const TextViewport& other) was hand-written once\n"
            "void TextViewport::Clear() {}\n");
  Expect(CheckTextViewportSpecialMembersAreDefaulted(root).violations.empty(),
         "defaulted special members (and a definition named only in a comment) must pass");

  WriteFile(root / "src/editor/TextViewport.cpp",
            "TextViewport::TextViewport(const TextViewport& other) : cursor_line_(1) {}\n"
            "TextViewport& TextViewport::operator=(TextViewport&& other) noexcept {\n"
            "  return *this;\n"
            "}\n");
  Expect(CheckTextViewportSpecialMembersAreDefaulted(root).violations.size() == 2,
         "each out-of-line copy/move definition must be flagged");

  WriteFile(root / "src/editor/TextViewport.cpp", "void TextViewport::Clear() {}\n");
  WriteFile(root / "src/editor/TextViewport.h",
            "class TextViewport {\n"
            "  TextViewport(const TextViewport& other);\n"
            "};\n");
  Expect(CheckTextViewportSpecialMembersAreDefaulted(root).violations.size() == 4,
         "a header without the four defaulted declarations must be flagged for each");
}

void RunSdlDrawAllowlistRuleFixtures() {
  TemporaryDirectory dir;
  const std::filesystem::path& root = dir.path();
  for (const std::string_view entry : SdlDrawAllowlist()) {
    std::filesystem::create_directories((root / entry).parent_path());
    WriteFile(root / entry, "void Draw(SDL_Renderer* r, const SDL_FRect* f) { SDL_RenderFillRect(r, f); }\n");
  }
  std::filesystem::create_directories(root / "src/workspace/render");
  WriteFile(root / "src/workspace/render/WorkspaceShellRenderNew.cpp",
            "void A(SDL_Renderer* r, const SDL_FRect* f) { SDL_RenderFillRect(r, f); }\n"
            "void B(SDL_Renderer* r) { SDL_RenderLine(r, 0, 0, 1, 1); }\n"
            "void C(SDL_Renderer* r, SDL_Texture* t) { SDL_RenderTexture(r, t, nullptr, nullptr); }\n");
  Expect(CheckSdlDrawCallsStayInTheAllowlist(root).violations.size() == 3,
         "every direct SDL draw call outside the allowlist must be flagged");

  WriteFile(root / "src/workspace/render/WorkspaceShellRenderNew.cpp",
            "void A(SDL_Renderer* r, const SDL_FRect& f) { render::FillRect(r, f, {}); }\n"
            "// SDL_RenderFillRect(r, f) is what this file must not do\n"
            "const char* kNote = \"SDL_RenderLine(r, 0, 0, 1, 1)\";\n"
            "void B(SDL_Renderer* r) { SDL_SetRenderClipRect(r, nullptr); }\n");
  Expect(CheckSdlDrawCallsStayInTheAllowlist(root).violations.empty(),
         "primitives, comments, strings and non-draw SDL calls are not violations");

  WriteFile(root / SdlDrawAllowlist().front(), "void Draw() {}\n");
  Expect(!CheckSdlDrawCallsStayInTheAllowlist(root).violations.empty(),
         "an allowlisted file that no longer draws must fail: the list cannot go stale");
}

void RunAllRuleFixtures() {
  RunDecoratedGridBatchesFillsRuleFixtures();
  RunExecutorPostThenFutureGetRuleFixtures();
  RunDescriptorCloseOnExecRuleFixtures();
  RunSdlDrawAllowlistRuleFixtures();
  RunTerminalExtractedImplRuleFixtures();
  RunDirectGitRepositoryRuleFixtures();
  RunActionIdReachabilityRuleFixtures();
  RunRegisteredSettingsAreReadRuleFixtures();
  RunCoordinatorOperationsAreCalledRuleFixtures();
  RunRenderTuTextCompositionRuleFixtures();
  RunPaintedScrollbarRuleFixtures();
  RunWheelFocusRuleFixtures();
  RunQueuedModifiersRuleFixtures();
  RunMissingRuleTargetFixtures();
  RunPerfCounterProducerRuleFixtures();
  RunEditorGroupsSplitTreeRuleFixtures();
  RunViewportFiletypeMemoRuleFixtures();
  RunPerfHarnessIsolationOrderRuleFixtures();
  RunPerfMeasureBodyRuleFixtures();
  RunPerfMeasureWallClockWaitRuleFixtures();
  RunShellGlueBindingRuleFixtures();
  RunFactoryCaptureRuleFixtures();
  RunHintSeparatorRuleFixtures();
  RunSideNameRuleFixtures();
  RunDerivedSurfacePaintRuleFixtures();
  RunKernelWindowingLibraryRuleFixtures();
  RunProcessLauncherRuleFixtures();
  RunWorkerDeclarationOrderRuleFixtures();
  RunGitMetadataRuleFixtures();
  RunGitLayerLocalityRuleFixtures();
  RunWriteGateRuleFixtures();
  RunTextViewportDefaultedSpecialMembersRuleFixtures();
  RunRangeForTemporaryRuleFixtures();
  RunEditorMetricsStickyBandRuleFixtures();
  RunNoCommentsRuleFixtures();
  RunTabSurfacesRuleFixtures();
  RunRelativeClimbOutRuleFixtures();
  RunWaitFailureMessageRuleFixtures();
}

}
