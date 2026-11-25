#pragma once

#include <optional>
#include <string>
#include <vector>

#include "project/ProjectSearchService.h"
#include "util/JsonValue.h"

namespace microide::project::remote {

util::JsonValue SearchRunParams(const std::string& query, const ProjectSearchOptions& options);
std::optional<std::pair<std::string, ProjectSearchOptions>> ParseSearchRunParams(
    const util::JsonValue& params, std::string* error);

util::JsonValue SearchResultsJson(const std::vector<ProjectSearchResult>& results,
                                  std::size_t searched_files, std::size_t total_files);
bool ParseSearchResults(const util::JsonValue& params, std::vector<ProjectSearchResult>& out,
                        std::size_t* searched_files, std::size_t* total_files);

util::JsonValue SearchCompletionJson(const ProjectSearchCompletion& completion);
ProjectSearchCompletion ParseSearchCompletion(const util::JsonValue& result);

}
