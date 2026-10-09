#pragma once

#include <optional>
#include <string>
#include <vector>

#include "project/ProjectSearchService.h"
#include "util/JsonValue.h"

namespace microide::project::remote {

// search/run (remote-projects.md § 6.11): the client sends the query and its
// options; the host runs RunProjectSearch over its content set, streams
// `search/results` notifications (frame id = the request) on the bulk lane, and
// answers with the completion on the same lane, after the last of them.
util::JsonValue SearchRunParams(const std::string& query, const ProjectSearchOptions& options);
// Untrusted: nullopt with *error on anything malformed.
std::optional<std::pair<std::string, ProjectSearchOptions>> ParseSearchRunParams(
    const util::JsonValue& params, std::string* error);

util::JsonValue SearchResultsJson(const std::vector<ProjectSearchResult>& results,
                                  std::size_t searched_files, std::size_t total_files);
// Appends to `out`; false on a malformed batch (it is then dropped whole).
bool ParseSearchResults(const util::JsonValue& params, std::vector<ProjectSearchResult>& out,
                        std::size_t* searched_files, std::size_t* total_files);

util::JsonValue SearchCompletionJson(const ProjectSearchCompletion& completion);
ProjectSearchCompletion ParseSearchCompletion(const util::JsonValue& result);

}  // namespace microide::project::remote
