#include "project/remote/RemoteSearchWire.h"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace microide::project::remote {
namespace {

constexpr std::size_t kMaxQueryBytes = 64 * 1024;
constexpr std::size_t kMaxGlobBytes = 64 * 1024;
constexpr std::size_t kMaxResultsPerBatch = 4096;
constexpr std::size_t kMaxPreviewBytes = 4096;

util::JsonValue Size(std::size_t value) {
  return util::JsonValue(static_cast<std::int64_t>(value));
}

std::optional<std::size_t> ReadSize(const util::JsonValue& value) {
  if (!value.IsInt() || value.AsInt() < 0) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(value.AsInt());
}

// A host's relative path must stay relative and inside the tree: it becomes a
// path under the mirror on this side.
bool SafeRelativePath(const std::string& path) {
  if (path.empty() || path.front() == '/' || path.find('\0') != std::string::npos) {
    return false;
  }
  std::size_t start = 0;
  while (start <= path.size()) {
    const std::size_t end = std::min(path.find('/', start), path.size());
    if (path.compare(start, end - start, "..") == 0 && end - start == 2) {
      return false;
    }
    start = end + 1;
  }
  return true;
}

}  // namespace

util::JsonValue SearchRunParams(const std::string& query, const ProjectSearchOptions& options) {
  util::JsonObject params;
  params["query"] = util::JsonValue(query);
  params["regex"] = util::JsonValue(options.pattern_mode == ProjectSearchPatternMode::Regex);
  params["case"] = util::JsonValue(std::string(
      options.case_mode == ProjectSearchCaseMode::Sensitive     ? "sensitive"
      : options.case_mode == ProjectSearchCaseMode::Insensitive ? "insensitive"
                                                                 : "smart"));
  params["show_hidden"] = util::JsonValue(options.show_hidden);
  params["count_all"] = util::JsonValue(options.count_all_matches);
  params["include"] = util::JsonValue(options.include_globs);
  params["exclude"] = util::JsonValue(options.exclude_globs);
  return util::JsonValue(std::move(params));
}

std::optional<std::pair<std::string, ProjectSearchOptions>> ParseSearchRunParams(
    const util::JsonValue& params, std::string* error) {
  const auto fail = [&](std::string message) -> std::optional<std::pair<std::string, ProjectSearchOptions>> {
    if (error != nullptr) {
      *error = std::move(message);
    }
    return std::nullopt;
  };
  if (!params.IsObject()) {
    return fail("search/run params must be an object");
  }
  const util::JsonValue& query = params["query"];
  if (!query.IsString() || query.AsString().size() > kMaxQueryBytes) {
    return fail("query must be a string of at most 64 KiB");
  }
  ProjectSearchOptions options;
  options.pattern_mode = params["regex"].AsBool(false) ? ProjectSearchPatternMode::Regex
                                                       : ProjectSearchPatternMode::Literal;
  const std::string& mode = params["case"].AsString();
  options.case_mode = mode == "sensitive"     ? ProjectSearchCaseMode::Sensitive
                      : mode == "insensitive" ? ProjectSearchCaseMode::Insensitive
                                              : ProjectSearchCaseMode::Smart;
  options.show_hidden = params["show_hidden"].AsBool(false);
  options.count_all_matches = params["count_all"].AsBool(false);
  for (auto [key, into] : {std::pair{"include", &options.include_globs},
                           std::pair{"exclude", &options.exclude_globs}}) {
    const util::JsonValue& globs = params[key];
    if (!globs.IsNull() && (!globs.IsString() || globs.AsString().size() > kMaxGlobBytes)) {
      return fail(std::string(key) + " must be a string of at most 64 KiB");
    }
    *into = globs.IsString() ? globs.AsString() : std::string();
  }
  return std::pair{query.AsString(), std::move(options)};
}

util::JsonValue SearchResultsJson(const std::vector<ProjectSearchResult>& results,
                                  std::size_t searched_files, std::size_t total_files) {
  util::JsonArray rows;
  rows.reserve(results.size());
  for (const ProjectSearchResult& result : results) {
    util::JsonObject row;
    row["path"] = util::JsonValue(result.relative_path_string);
    row["file_index"] = Size(result.file_index);
    row["line"] = Size(result.line);
    row["column"] = Size(result.column);
    row["preview"] = util::JsonValue(result.preview);
    row["match_start"] = Size(result.match_preview_start);
    row["match_length"] = Size(result.match_preview_length);
    rows.push_back(util::JsonValue(std::move(row)));
  }
  util::JsonObject params;
  params["results"] = util::JsonValue(std::move(rows));
  params["searched"] = Size(searched_files);
  params["total"] = Size(total_files);
  return util::JsonValue(std::move(params));
}

bool ParseSearchResults(const util::JsonValue& params, std::vector<ProjectSearchResult>& out,
                        std::size_t* searched_files, std::size_t* total_files) {
  const util::JsonValue& rows = params["results"];
  const auto searched = ReadSize(params["searched"]);
  const auto total = ReadSize(params["total"]);
  if (!rows.IsArray() || rows.AsArray().size() > kMaxResultsPerBatch || !searched || !total) {
    return false;
  }
  std::vector<ProjectSearchResult> batch;
  batch.reserve(rows.AsArray().size());
  for (const util::JsonValue& row : rows.AsArray()) {
    const util::JsonValue& path = row["path"];
    const util::JsonValue& preview = row["preview"];
    const auto file_index = ReadSize(row["file_index"]);
    const auto line = ReadSize(row["line"]);
    const auto column = ReadSize(row["column"]);
    const auto match_start = ReadSize(row["match_start"]);
    const auto match_length = ReadSize(row["match_length"]);
    if (!path.IsString() || !SafeRelativePath(path.AsString()) || !preview.IsString() ||
        preview.AsString().size() > kMaxPreviewBytes || !file_index || !line || !column ||
        !match_start || !match_length || *match_start > preview.AsString().size() ||
        *match_length > preview.AsString().size() - *match_start) {
      return false;
    }
    ProjectSearchResult& result = batch.emplace_back();
    result.relative_path_string = path.AsString();
    result.relative_path = std::filesystem::path(result.relative_path_string);
    result.file_index = *file_index;
    result.line = *line;
    result.column = *column;
    result.preview = preview.AsString();
    result.match_preview_start = *match_start;
    result.match_preview_length = *match_length;
  }
  out.insert(out.end(), std::make_move_iterator(batch.begin()), std::make_move_iterator(batch.end()));
  *searched_files = *searched;
  *total_files = *total;
  return true;
}

util::JsonValue SearchCompletionJson(const ProjectSearchCompletion& completion) {
  util::JsonObject result;
  result["error"] = util::JsonValue(completion.error);
  result["truncated"] = util::JsonValue(completion.truncated);
  result["total_matches"] = Size(completion.total_matches);
  return util::JsonValue(std::move(result));
}

ProjectSearchCompletion ParseSearchCompletion(const util::JsonValue& result) {
  ProjectSearchCompletion completion;
  completion.error = result["error"].AsString();
  completion.truncated = result["truncated"].AsBool(false);
  completion.total_matches = ReadSize(result["total_matches"]).value_or(0);
  return completion;
}

}  // namespace microide::project::remote
