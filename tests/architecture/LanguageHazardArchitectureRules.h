#pragma once

#include "architecture/ArchitectureRuleHelpers.h"

#include <filesystem>

namespace microide::tests::architecture {

RuleResult CheckRangeForDoesNotSubscriptATemporary(const std::filesystem::path& repo_root);

void RunRangeForTemporaryRuleFixtures();

RuleResult CheckRelativeClimbOutTestsTheFirstComponent(const std::filesystem::path& repo_root);
void RunRelativeClimbOutRuleFixtures();

RuleResult CheckWaitFailureMessagesAreLazy(const std::filesystem::path& repo_root);
void RunWaitFailureMessageRuleFixtures();

}
