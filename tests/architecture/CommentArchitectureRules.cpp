#include "architecture/CommentArchitectureRules.h"

#include "TestSupport.h"
#include "architecture/ArchitectureFileScanner.h"

#include <algorithm>
#include <array>
#include <regex>
#include <string>

namespace microide::tests::architecture {
namespace {

constexpr std::array<std::string_view, 6> kSourceExtensions = {".cpp", ".h", ".inc", ".hpp", ".c", ".cc"};
constexpr std::array<std::string_view, 3> kScannedRoots = {"src", "tests", "tools"};
constexpr std::array<std::string_view, 2> kDataDirectories = {"tests/fixtures", "tests/perf/fixtures"};
constexpr std::array<std::string_view, 2> kGeneratedFiles = {"src/util/CaseFoldTable.inc",
                                                             "src/util/DisplayWidthTable.inc"};

bool IsIdentChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

std::size_t SkipQuoted(std::string_view text, std::size_t open, char quote) {
  std::size_t i = open + 1;
  while (i < text.size() && text[i] != quote && text[i] != '\n') {
    i += text[i] == '\\' ? 2 : 1;
  }
  return std::min(i + 1, text.size());
}

std::size_t SkipRawString(std::string_view text, std::size_t quote) {
  const std::size_t paren = text.find('(', quote + 1);
  if (paren == std::string_view::npos) {
    return text.size();
  }
  const std::string close = ")" + std::string(text.substr(quote + 1, paren - quote - 1)) + "\"";
  const std::size_t end = text.find(close, paren + 1);
  return end == std::string_view::npos ? text.size() : end + close.size();
}

bool StartsRawString(std::string_view text, std::size_t quote) {
  std::size_t start = quote;
  while (start > 0 && IsIdentChar(text[start - 1])) {
    --start;
  }
  const std::string_view prefix = text.substr(start, quote - start);
  return prefix == "R" || prefix == "u8R" || prefix == "uR" || prefix == "UR" || prefix == "LR";
}

bool IsDigitSeparator(std::string_view text, std::size_t quote) {
  std::size_t start = quote;
  while (start > 0 && (IsIdentChar(text[start - 1]) || text[start - 1] == '.' || text[start - 1] == '\'')) {
    --start;
  }
  return start < quote && (text[start] >= '0' && text[start] <= '9');
}

bool IsAllowedDirective(std::string_view comment) {
  static const std::regex allowed(R"(^//\s*(NOLINT|IWYU pragma|perf-measure-(builds-input|waits-on-clock):))");
  return std::regex_search(comment.begin(), comment.end(), allowed);
}

bool IsExcluded(const std::filesystem::path& relative) {
  const std::string generic = relative.generic_string();
  for (const std::string_view dir : kDataDirectories) {
    if (generic.starts_with(std::string(dir) + "/")) {
      return true;
    }
  }
  return std::find(kGeneratedFiles.begin(), kGeneratedFiles.end(), generic) != kGeneratedFiles.end();
}

}

std::vector<SourceComment> FindSourceComments(std::string_view text) {
  std::vector<SourceComment> comments;
  std::size_t i = 0;
  while (i < text.size()) {
    const char c = text[i];
    const char next = i + 1 < text.size() ? text[i + 1] : '\0';
    if (c == '/' && next == '/') {
      const std::size_t end = std::min(text.find('\n', i), text.size());
      comments.push_back({.offset = i, .text = text.substr(i, end - i)});
      i = end;
    } else if (c == '/' && next == '*') {
      const std::size_t close = text.find("*/", i + 2);
      const std::size_t end = close == std::string_view::npos ? text.size() : close + 2;
      comments.push_back({.offset = i, .text = text.substr(i, end - i)});
      i = end;
    } else if (c == '"') {
      i = StartsRawString(text, i) ? SkipRawString(text, i) : SkipQuoted(text, i, '"');
    } else if (c == '\'' && !IsDigitSeparator(text, i)) {
      i = SkipQuoted(text, i, '\'');
    } else {
      ++i;
    }
  }
  return comments;
}

RuleResult CheckNoCommentsInSources(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "source files carry no comments, only tool directives";
  result.hard_fail = true;
  if (!RequireRuleTarget(result, repo_root / "src")) {
    return result;
  }
  std::size_t scanned = 0;
  for (const std::string_view root : kScannedRoots) {
    const std::filesystem::path dir = repo_root / root;
    if (!std::filesystem::is_directory(dir)) {
      continue;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
      const std::string extension = entry.path().extension().string();
      if (!entry.is_regular_file() ||
          std::find(kSourceExtensions.begin(), kSourceExtensions.end(), extension) == kSourceExtensions.end() ||
          IsExcluded(std::filesystem::relative(entry.path(), repo_root))) {
        continue;
      }
      ++scanned;
      const std::string text = ReadText(entry.path());
      for (const SourceComment& comment : FindSourceComments(text)) {
        if (IsAllowedDirective(comment.text)) {
          continue;
        }
        result.violations.push_back(Violation{
            .path = entry.path(),
            .line = LineNumberAt(text, comment.offset),
            .message = "comment in source: say it in names, types and small functions, and put the why "
                       "in the commit message or dev-docs/ (only NOLINT, IWYU pragma and perf-measure-*: "
                       "directives are allowed)",
        });
      }
    }
  }
  if (scanned == 0) {
    result.missing_targets.push_back(Violation{
        .path = repo_root / "src",
        .line = 0,
        .message = "rule target moved -- no source file found to scan; re-anchor CheckNoCommentsInSources"});
  }
  return result;
}

void RunNoCommentsRuleFixtures() {
  TemporaryDirectory fixture;
  const std::filesystem::path& root = fixture.path();
  std::filesystem::create_directories(root / "src");
  WriteFile(root / "src/Bad.cpp",
            "// a line comment\n"
            "int a = 1;  // a trailing one\n"
            "/* a block\n   over two lines */\n"
            "void F(bool) {}\n"
            "void G() { F(/*flag=*/true); }\n");
  RuleResult bad = CheckNoCommentsInSources(root);
  Expect(bad.violations.size() == 4, "the comment rule must flag line, trailing, block and inline comments");
  Expect(bad.violations.size() == 4 && bad.violations[2].line == 3,
         "the comment rule must report the line a comment starts on");

  std::filesystem::remove(root / "src/Bad.cpp");
  std::filesystem::create_directories(root / "tests/fixtures");
  WriteFile(root / "tests/fixtures/Data.cpp", "// fixture data keeps its comments\n");
  WriteFile(root / "src/Good.cpp",
            "#include <string_view>  // IWYU pragma: keep\n"
            "int a = 1'000'000;  // NOLINT(readability-magic-numbers)\n"
            "char slash = '/';\n"
            "char quote = '\\'';\n"
            "const char* s = \"// not a comment /* either */\";\n"
            "const char* r = R\"cpp(// inside a raw string\n/* too */)cpp\";\n"
            "const char* u = u8R\"x(\")// still raw\")x\";\n"
            "// perf-measure-waits-on-clock: the wait is the measurement\n"
            "int b = a / 2;\n");
  RuleResult good = CheckNoCommentsInSources(root);
  Expect(good.violations.empty() && good.missing_targets.empty(),
         "the comment rule must accept tool directives, literals that look like comments, and fixture data");

  TemporaryDirectory empty;
  Expect(!CheckNoCommentsInSources(empty.path()).missing_targets.empty(),
         "the comment rule must fail loudly when there is no source tree to scan");
}

}
