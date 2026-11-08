#include "architecture/InputGeometryArchitectureRules.h"

#include "TestSupport.h"
#include "architecture/ArchitectureFileScanner.h"

#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace microide::tests::architecture {
namespace {

// How far above a call its explanation may sit: the comment block directly over
// the statement, which in this tree runs to a handful of lines.
constexpr std::size_t kCommentWindowLines = 8;

// The fourth argument of the call whose `(` is at `open`, or "" when it cannot
// be read (fewer arguments, unbalanced text).
std::string FourthArgument(std::string_view code, std::size_t open) {
  int depth = 0;
  std::size_t argument = 0;
  std::size_t start = open + 1;
  for (std::size_t i = open; i < code.size(); ++i) {
    const char c = code[i];
    if (c == '(' || c == '[' || c == '{') {
      ++depth;
    } else if (c == ')' || c == ']' || c == '}') {
      if (--depth == 0) {
        return argument == 3 ? std::string(code.substr(start, i - start)) : std::string();
      }
    } else if (c == ',' && depth == 1) {
      if (argument == 3) {
        return std::string(code.substr(start, i - start));
      }
      ++argument;
      start = i + 1;
    }
  }
  return {};
}

std::string Trimmed(std::string text) {
  const auto first = text.find_first_not_of(" \t\r\n");
  const auto last = text.find_last_not_of(" \t\r\n");
  return first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
}

}  // namespace

RuleResult CheckEditorMetricsNameTheirStickyBand(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "an editor metrics call with a literal 0 sticky band says why the band does not matter";
  result.hard_fail = true;
  const std::filesystem::path dir = repo_root / "src" / "workspace";
  if (!RequireRuleTarget(result, dir)) {
    return result;
  }
  const std::regex call(R"(\bComputeMetrics\s*\()");
  std::size_t calls = 0;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
    const std::filesystem::path extension = entry.path().extension();
    if (!entry.is_regular_file() || (extension != ".cpp" && extension != ".h" && extension != ".inc")) {
      continue;
    }
    // The TestAccess seams size panes for tests on purpose, band or not.
    if (entry.path().parent_path().filename() == "testaccess") {
      continue;
    }
    const std::string text = ReadText(entry.path());
    const std::vector<bool> is_code = BuildCodeMask(text);
    std::string code = text;
    for (std::size_t i = 0; i < code.size() && i < is_code.size(); ++i) {
      if (!is_code[i] && code[i] != '\n') {
        code[i] = ' ';
      }
    }
    for (std::sregex_iterator it(code.begin(), code.end(), call), last; it != last; ++it) {
      const std::size_t open = static_cast<std::size_t>(it->position() + it->length()) - 1;
      const std::string band = Trimmed(FourthArgument(code, open));
      if (band.empty()) {
        continue;  // the declaration, or a call this scan cannot read
      }
      ++calls;
      if (band != "0") {
        continue;
      }
      // The explanation: a comment mentioning the band in the lines just above
      // the call's statement (searched in the ORIGINAL text, comments intact).
      const std::size_t line = LineNumberAt(text, static_cast<std::size_t>(it->position()));
      std::size_t begin = 0;
      for (std::size_t seen = 1; seen + kCommentWindowLines < line && begin < text.size(); ++seen) {
        begin = text.find('\n', begin) + 1;
      }
      const std::string_view window(text.data() + begin, static_cast<std::size_t>(it->position()) - begin);
      const bool explained = window.find("//") != std::string_view::npos &&
                             (window.find("band") != std::string_view::npos ||
                              window.find("sticky") != std::string_view::npos);
      if (!explained) {
        result.violations.push_back(Violation{
            .path = entry.path(),
            .line = line,
            .message = "ComputeMetrics with a literal 0 sticky band and no comment saying why the band "
                       "does not matter here: pass the viewport's sticky_band_rows(), or explain the 0 "
                       "(TD-2026-11-08-434's five sites laid rows out under a pinned band)",
        });
      }
    }
  }
  if (calls == 0) {
    result.missing_targets.push_back(Violation{
        .path = dir,
        .line = 0,
        .message = "rule target moved -- no ComputeMetrics call found to scan; re-anchor "
                   "CheckEditorMetricsNameTheirStickyBand"});
  }
  return result;
}

void RunEditorMetricsStickyBandRuleFixtures() {
  TemporaryDirectory fixture;
  const std::filesystem::path& root = fixture.path();
  std::filesystem::create_directories(root / "src" / "workspace");
  // Negative control: a bare 0, and a 0 whose comment says nothing of the band.
  WriteFile(root / "src/workspace/Bad.cpp",
            "void A(){ auto m = editor::EditorViewRenderer::ComputeMetrics(r, v, rect, 0, numbers); }\n"
            "// unrelated words\n"
            "void B(){ auto m = ComputeMetrics(\n    r, v, rect, 0, numbers); }\n");
  Expect(CheckEditorMetricsNameTheirStickyBand(root).violations.size() == 2,
         "the sticky-band rule must flag a literal 0 band with no explanation, on one line or two");
  // Positive control: the band passed, and a 0 explained just above it.
  WriteFile(root / "src/workspace/Bad.cpp",
            "void A(){ auto m = ComputeMetrics(r, v, rect, v.sticky_band_rows(), numbers); }\n"
            "void B(){\n"
            "  // Columns only: the sticky band takes rows, not columns.\n"
            "  auto m = ComputeMetrics(r, v, rect, 0, numbers);\n"
            "}\n"
            "void C(){ auto m = ComputeMetrics(r, v, rect, sticky_rows, numbers); }\n"
            "const char* kDoc = \"ComputeMetrics(r, v, rect, 0, n)\";\n");
  Expect(CheckEditorMetricsNameTheirStickyBand(root).violations.empty(),
         "the sticky-band rule must accept the band passed, a variable, an explained 0, and string text");
  // Loud-missing-target guard.
  WriteFile(root / "src/workspace/Bad.cpp", "int H(){ return 0; }\n");
  Expect(!CheckEditorMetricsNameTheirStickyBand(root).missing_targets.empty(),
         "the sticky-band rule must fail loudly when it finds no ComputeMetrics call");
}

}  // namespace microide::tests::architecture
