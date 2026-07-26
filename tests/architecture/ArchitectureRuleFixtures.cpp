#include "architecture/ArchitectureRuleFixtures.h"

#include "TestSupport.h"
#include "architecture/TerminalArchitectureRules.h"

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

}  // namespace microide::tests::architecture
