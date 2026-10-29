#include "architecture/LanguageHazardArchitectureRules.h"

#include "TestSupport.h"

#include <cctype>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace microide::tests::architecture {
namespace {

// Offset of the `:` that makes `head` (the text between a `for`'s parentheses) a
// range-for, or npos. Top level only: `::`, a `?:` inside a call, and the `;` of
// a classic for are not it.
std::size_t RangeForColon(std::string_view head) {
  int depth = 0;
  for (std::size_t i = 0; i < head.size(); ++i) {
    const char c = head[i];
    if (c == '(' || c == '[' || c == '{') {
      ++depth;
    } else if (c == ')' || c == ']' || c == '}') {
      --depth;
    } else if (depth == 0 && c == ';') {
      return std::string_view::npos;
    } else if (depth == 0 && c == ':') {
      if (i + 1 < head.size() && head[i + 1] == ':') {
        ++i;
        continue;
      }
      return i;
    }
  }
  return std::string_view::npos;
}

// Whether `range` subscripts the result of a call: a `)` followed by `[`, whose
// matching `(` follows a name or a template's `>` -- `F(x)[k]`, `F<T>()[0]`. A
// parenthesized expression (`(*json)["rows"]`) is not a call.
bool SubscriptsACallResult(std::string_view range) {
  for (std::size_t close = 0; close < range.size(); ++close) {
    if (range[close] != ')') {
      continue;
    }
    std::size_t next = close + 1;
    while (next < range.size() && std::isspace(static_cast<unsigned char>(range[next])) != 0) {
      ++next;
    }
    if (next >= range.size() || range[next] != '[') {
      continue;
    }
    int depth = 0;
    std::size_t open = close + 1;
    while (open-- > 0) {
      if (range[open] == ')') {
        ++depth;
      } else if (range[open] == '(' && --depth == 0) {
        break;
      }
    }
    if (open == static_cast<std::size_t>(-1) || open == 0) {
      continue;
    }
    std::size_t before = open;
    while (before-- > 0 && std::isspace(static_cast<unsigned char>(range[before])) != 0) {
    }
    if (before != static_cast<std::size_t>(-1) &&
        (std::isalnum(static_cast<unsigned char>(range[before])) != 0 || range[before] == '_' ||
         range[before] == '>')) {
      return true;
    }
  }
  return false;
}

}  // namespace

// The one shape of TD-2026-10-27-404 a scan can tell apart. The hazard in general
// is a member FUNCTION or `operator[]` returning a reference into a temporary
// returned by value; a regex cannot tell that from a call on a cached reference
// (`MakeEditorTabService().DirtyGroupTabs()` is fine) or a data member of the
// temporary (`: ComputeEditorGroupRects(layout).groups` extends the whole object,
// and is fine). A subscript of a call's result is the shape that slipped through
// -- a JSON lookup, `ControlUi(shell)["elements"].AsArray()`, passed every plain
// run and failed only under ASan -- and a call returning a reference it could
// subscript safely is rare enough that binding it to a named local first is the
// right spelling anyway. GCC's -Wdangling-reference did not fire on it, and the
// compilers CI builds with predate C++23's fix (P2718R0: GCC 15, Clang 19).
RuleResult CheckRangeForDoesNotSubscriptATemporary(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "a range-for must not iterate a subscript of a call's temporary result";
  result.hard_fail = true;
  const std::regex for_pattern(R"(\bfor\s*\()");
  std::size_t range_fors = 0;
  for (const char* top : {"src", "tests"}) {
    const std::filesystem::path dir = repo_root / top;
    if (!RequireRuleTarget(result, dir)) {
      continue;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
      const std::filesystem::path extension = entry.path().extension();
      if (!entry.is_regular_file() || (extension != ".cpp" && extension != ".h" && extension != ".inc")) {
        continue;
      }
      const std::string text = ReadText(entry.path());
      const std::vector<bool> is_code = BuildCodeMask(text);
      // The loop header with everything that is not code blanked, so a paren or
      // colon in a string literal or comment counts for nothing.
      std::string code = text;
      for (std::size_t i = 0; i < code.size() && i < is_code.size(); ++i) {
        if (!is_code[i] && code[i] != '\n') {
          code[i] = ' ';
        }
      }
      for (std::sregex_iterator it(code.begin(), code.end(), for_pattern), last; it != last; ++it) {
        const std::size_t open = static_cast<std::size_t>(it->position() + it->length()) - 1;
        std::size_t close = open;
        for (int depth = 0; close < code.size(); ++close) {
          if (code[close] == '(') {
            ++depth;
          } else if (code[close] == ')' && --depth == 0) {
            break;
          }
        }
        if (close >= code.size()) {
          continue;
        }
        const std::string_view head(code.data() + open + 1, close - open - 1);
        const std::size_t colon = RangeForColon(head);
        if (colon == std::string_view::npos) {
          continue;
        }
        ++range_fors;
        if (SubscriptsACallResult(head.substr(colon + 1))) {
          result.violations.push_back(Violation{
              .path = entry.path(),
              .line = LineNumberAt(text, open),
              .message = "range-for over a subscript of a call's result: when the call returns by "
                         "value, the loop walks freed storage (only the range expression's own "
                         "result is lifetime-extended). Bind the call's result to a named local "
                         "first (TD-2026-10-27-404)",
          });
        }
      }
    }
  }
  if (range_fors == 0) {
    result.violations.push_back(Violation{
        .path = repo_root / "src",
        .line = 1,
        .message = "rule target moved -- no range-for loops found to scan; re-anchor "
                   "CheckRangeForDoesNotSubscriptATemporary",
    });
  }
  return result;
}

void RunRangeForTemporaryRuleFixtures() {
  TemporaryDirectory fixture;
  const std::filesystem::path& root = fixture.path();
  std::filesystem::create_directories(root / "src");
  std::filesystem::create_directories(root / "tests");

  WriteFile(root / "tests/Bad.cpp",
            "void A(){ for (const auto& e : TestAccess::ControlUi(shell)[\"elements\"].AsArray()) {} }\n"
            "void B(){ for (int x : Make<int>()[0]) {} }\n"
            "void C(){ for (auto& x : Rows(a, f(b))\n    [2]) {} }\n");
  WriteFile(root / "src/Good.cpp", "void G(){ for (int i = 0; i < 3; ++i) {} }\n");
  Expect(CheckRangeForDoesNotSubscriptATemporary(root).violations.size() == 3,
         "the range-for rule must flag a subscript of a call's result, a template call's, and one "
         "wrapped across lines");

  // Positive control: the safe spellings next to it.
  WriteFile(root / "tests/Bad.cpp",
            "void A(){ const auto ui = TestAccess::ControlUi(shell); for (const auto& e : ui[\"elements\"].AsArray()) {} }\n"
            "void B(){ for (auto& g : ComputeEditorGroupRects(layout).groups) {} }\n"
            "void C(){ for (auto& t : MakeEditorTabService().DirtyGroupTabs()) {} }\n"
            "void D(){ for (auto& r : (*json)[\"rows\"].AsArray()) {} }\n"
            "void E(){ for (std::size_t i = 0; i < F()[1]; ++i) {} }\n"
            "void F(){ for (auto& x : std::vector<int>{1, 2}) {} }\n"
            "void H(){ for (auto& x : ns::Table::Rows()) {} }\n"
            "const char* kDoc = \"for (auto& e : F()[0])\";\n"
            "// for (auto& e : F()[0]) is documented here, not written\n");
  Expect(CheckRangeForDoesNotSubscriptATemporary(root).violations.empty(),
         "the range-for rule must accept a named local, a data member, a member call, a "
         "parenthesized subscript, a classic for, and string or comment text");

  // Loud-missing-target guard: no range-for at all is a hollowed-out rule.
  WriteFile(root / "tests/Bad.cpp", "int H(){ return 0; }\n");
  Expect(!CheckRangeForDoesNotSubscriptATemporary(root).violations.empty(),
         "the range-for rule must fail loudly when it finds no range-for to scan");
}

RuleResult CheckRelativeClimbOutTestsTheFirstComponent(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "a relative path climbs out by its first component, not by a \"..\" text prefix";
  result.hard_fail = true;
  // The literal is not code (BuildCodeMask), so the match is anchored on the call's
  // `.` -- which is -- and a mention in a comment or a string is not a call.
  const std::regex prefix_test(R"re(\.(?:starts_with\(\s*"\.\."\s*\)|rfind\(\s*"\.\."\s*,\s*0\s*\)))re");
  const std::filesystem::path helper = repo_root / "src/util/PathMatch.h";
  if (!RequireRuleTarget(result, helper)) {
    return result;
  }
  if (ReadText(helper).find("RelativePathClimbsOut") == std::string::npos) {
    result.violations.push_back(Violation{
        .path = helper,
        .line = 1,
        .message = "rule target moved -- util::RelativePathClimbsOut is gone; re-anchor "
                   "CheckRelativeClimbOutTestsTheFirstComponent",
    });
  }
  const std::filesystem::path dir = repo_root / "src";
  if (!RequireRuleTarget(result, dir)) {
    return result;
  }
  for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
    const std::filesystem::path extension = entry.path().extension();
    if (!entry.is_regular_file() || (extension != ".cpp" && extension != ".h" && extension != ".inc")) {
      continue;
    }
    const std::string text = ReadText(entry.path());
    if (text.find("\"..\"") == std::string::npos) {
      continue;
    }
    const std::vector<bool> is_code = BuildCodeMask(text);
    for (std::sregex_iterator it(text.begin(), text.end(), prefix_test), last; it != last; ++it) {
      const auto at = static_cast<std::size_t>(it->position());
      if (at >= is_code.size() || !is_code[at]) {
        continue;
      }
      result.violations.push_back(Violation{
          .path = entry.path(),
          .line = LineNumberAt(text, at),
          .message = "a \"..\" text-prefix test reads a child named `..cache` as outside its base; "
                     "use util::RelativePathClimbsOut / RelativeTextClimbsOut (TD-2026-10-29-423)",
      });
    }
  }
  return result;
}

void RunRelativeClimbOutRuleFixtures() {
  TemporaryDirectory fixture;
  const std::filesystem::path& root = fixture.path();
  std::filesystem::create_directories(root / "src/util");
  WriteFile(root / "src/util/PathMatch.h", "inline bool RelativePathClimbsOut(const path& p);\n");

  WriteFile(root / "src/Bad.cpp",
            "bool A(const path& r){ return r.native().starts_with(\"..\"); }\n"
            "bool B(const std::string& t){ return t.rfind(\"..\", 0) == 0; }\n"
            "bool C(const path& r){ return r.string().starts_with( \"..\" ); }\n");
  Expect(CheckRelativeClimbOutTestsTheFirstComponent(root).violations.size() == 3,
         "the climb-out rule must flag starts_with(\"..\") and rfind(\"..\", 0)");

  // Positive control: the helpers, a "../" prefix (a separator makes it a component
  // test), an unanchored rfind, and the text in a comment or a string.
  WriteFile(root / "src/Bad.cpp",
            "bool A(const path& r){ return util::RelativePathClimbsOut(r); }\n"
            "bool B(std::string_view t){ return t == \"..\" || t.starts_with(\"../\"); }\n"
            "std::size_t C(const std::string& t){ return t.rfind(\"..\"); }\n"
            "// r.native().starts_with(\"..\") is how it was written before\n"
            "const char* kDoc = \"t.rfind(\\\"..\\\", 0) == 0\";\n");
  Expect(CheckRelativeClimbOutTestsTheFirstComponent(root).violations.empty(),
         "the climb-out rule must accept the helpers, a \"../\" prefix, an unanchored rfind, and "
         "comment or string text");

  // Loud-missing-target guard: the helper the message names must exist.
  WriteFile(root / "src/util/PathMatch.h", "// nothing here\n");
  Expect(!CheckRelativeClimbOutTestsTheFirstComponent(root).violations.empty(),
         "the climb-out rule must fail loudly when util::RelativePathClimbsOut is gone");
}

}  // namespace microide::tests::architecture
