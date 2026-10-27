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

}  // namespace microide::tests::architecture
