#include "TestSupport.h"

#include <string>
#include <string_view>
#include <vector>

#include "workspace/TypeNavigation.h"

namespace microide::tests {
namespace {

using workspace::TypeNavigate;
using workspace::TypeNavigationState;

const std::vector<std::string> kRows = {"src", "scheduler.cpp", "main.cpp", "README.md", "Setup.py"};

std::optional<std::size_t> Type(TypeNavigationState& state, char c, std::uint64_t ns, std::size_t current) {
  return TypeNavigate(state, c, ns, current, kRows.size(),
                      [](std::size_t i) { return std::string_view(kRows[i]); });
}

void TestTypingSelectsTheNextMatchingRow() {
  TypeNavigationState state;
  Expect(Type(state, 'm', 0, 0) == std::optional<std::size_t>(2), "m selects main.cpp");
  Expect(Type(state, 'r', 2'000'000'000, 2) == std::optional<std::size_t>(3),
         "after a pause, r starts over and selects README.md (case-insensitive)");
}

void TestAPrefixRefinesInPlace() {
  TypeNavigationState state;
  Expect(Type(state, 's', 0, 0) == std::optional<std::size_t>(1), "s moves past src to scheduler.cpp");
  Expect(Type(state, 'e', 100, 1) == std::optional<std::size_t>(4),
         "se selects Setup.py (scheduler.cpp does not start with se)");
}

void TestARepeatedLetterCycles() {
  TypeNavigationState state;
  Expect(Type(state, 's', 0, 3) == std::optional<std::size_t>(4), "s from README selects Setup.py");
  Expect(Type(state, 's', 100, 4) == std::optional<std::size_t>(0), "s again wraps to src");
  Expect(Type(state, 's', 200, 0) == std::optional<std::size_t>(1), "and again to scheduler.cpp");
}

void TestNoMatchKeepsTheSelection() {
  TypeNavigationState state;
  Expect(!Type(state, 'z', 0, 2).has_value(), "no row starts with z");
  Expect(!TypeNavigate(state, 'a', 0, 0, 0, [](std::size_t) { return std::string_view(); }).has_value(),
         "an empty list selects nothing");
}

}  // namespace

void RegisterTypeNavigationTests(std::vector<TestCase>& tests) {
  AddTest(tests, "TypeNavigation/TypingSelectsTheNextMatchingRow", TestTypingSelectsTheNextMatchingRow);
  AddTest(tests, "TypeNavigation/APrefixRefinesInPlace", TestAPrefixRefinesInPlace);
  AddTest(tests, "TypeNavigation/ARepeatedLetterCycles", TestARepeatedLetterCycles);
  AddTest(tests, "TypeNavigation/NoMatchKeepsTheSelection", TestNoMatchKeepsTheSelection);
}

}  // namespace microide::tests
