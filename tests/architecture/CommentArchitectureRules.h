#pragma once

#include "architecture/ArchitectureRuleHelpers.h"

#include <cstddef>
#include <filesystem>
#include <string_view>
#include <vector>

namespace microide::tests::architecture {

struct SourceComment {
  std::size_t offset = 0;
  std::string_view text;
};

std::vector<SourceComment> FindSourceComments(std::string_view text);

RuleResult CheckNoCommentsInSources(const std::filesystem::path& repo_root);
void RunNoCommentsRuleFixtures();

}
