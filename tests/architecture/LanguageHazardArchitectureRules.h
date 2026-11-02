#pragma once

#include "architecture/ArchitectureRuleHelpers.h"

#include <filesystem>

namespace microide::tests::architecture {

// A range-for whose range is a subscript of a call's result -- `: F(x)[k]...` --
// iterates freed storage when F returns by value: C++20 extends the lifetime of
// the range expression's result, not of the temporary F returned that `[k]`
// reaches into (TD-2026-10-27-404). See the rule body for why this one shape.
RuleResult CheckRangeForDoesNotSubscriptATemporary(const std::filesystem::path& repo_root);

// Negative + positive control for the rule above.
void RunRangeForTemporaryRuleFixtures();

// "Does this relative path climb out?" asked as a text prefix -- `starts_with("..")`,
// `rfind("..", 0) == 0` -- reads a child named `..cache` as outside; nine sites did,
// the tree's copy guard among them (TD-2026-10-29-423). util::RelativePathClimbsOut /
// RelativeTextClimbsOut test the first component.
RuleResult CheckRelativeClimbOutTestsTheFirstComponent(const std::filesystem::path& repo_root);
void RunRelativeClimbOutRuleFixtures();

// `Expect(PumpUntil(...), "..." + Describe(x))` builds its failure text BEFORE the
// wait runs (the order of a call's argument evaluations is unspecified, and GCC
// goes right to left), so a failure shows the state from before the wait: a remote
// test's "panes:" dump read "[not connected ...]" for panes that had connected
// (TD-2026-11-02-429 item 7). The message of a wait that describes state with a
// call is a lambda: `[&] { return "..." + Describe(x); }`, built only on failure.
RuleResult CheckWaitFailureMessagesAreLazy(const std::filesystem::path& repo_root);
void RunWaitFailureMessageRuleFixtures();

}  // namespace microide::tests::architecture
