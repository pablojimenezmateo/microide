#include "workspace/ProjectSearchLiveHits.h"

#include <algorithm>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "project/GlobMatch.h"
#include "project/ProjectSearchService.h"
#include "util/PathMatch.h"

namespace microide::workspace {

std::optional<LiveHitsRequest> PrepareLiveHitsRequest(const ProjectSearchState& search,
                                                      const std::filesystem::path& root,
                                                      const editor::TextViewport& viewport) {
  if (search.running || search.run_query.empty() || root.empty() || viewport.path().empty()) {
    return std::nullopt;
  }
  const std::filesystem::path relative = viewport.path().lexically_normal().lexically_relative(root.lexically_normal());
  if (relative.empty() || util::RelativePathClimbsOut(relative)) {
    return std::nullopt;
  }
  std::string relative_path = relative.generic_string();
  const project::GlobSet include = project::GlobSet::Parse(search.run_options.include_globs);
  const project::GlobSet exclude = project::GlobSet::Parse(search.run_options.exclude_globs);
  if ((!include.empty() && !include.Matches(relative_path)) || (!exclude.empty() && exclude.Matches(relative_path))) {
    return std::nullopt;
  }
  if (!project::ProjectSearchMatcher(search.run_query, search.run_options).valid()) {
    return std::nullopt;
  }
  return LiveHitsRequest{.key = {.document = viewport.document_identity(),
                                 .content_revision = viewport.content_revision(),
                                 .relative_path = std::move(relative_path),
                                 .query = search.run_query,
                                 .options = search.run_options},
                         .text = viewport.lines().SnapshotDocument()};
}

LiveHits SearchLiveHits(const LiveHitsRequest& request) {
  const project::ProjectSearchMatcher matcher(request.key.query, request.key.options);
  project::LineSearch search(matcher, request.key.relative_path, 0, project::kMaxProjectSearchResults);
  request.text.ForEachLine([&search](std::string_view line) { search.Add(line); });
  return LiveHits{.key = request.key, .hits = search.Take()};
}

bool SpliceLiveHits(ProjectSearchState& search, LiveHits live, const ProjectSearchCandidates& candidates) {
  if (search.running || search.run_query != live.key.query || search.run_options != live.key.options) {
    return false;
  }
  const std::string& relative_path = live.key.relative_path;
  std::vector<project::ProjectSearchResult>& hits = live.hits;
  auto& results = search.results;
  auto first = std::find_if(results.begin(), results.end(), [&](const project::ProjectSearchResult& hit) {
    return hit.relative_path_string == relative_path;
  });
  std::optional<std::size_t> file_index;
  if (first != results.end()) {
    file_index = first->file_index;
    for (project::ProjectSearchResult& hit : hits) {
      hit.file_index = *file_index;
    }
  }
  if (!file_index.has_value()) {
    if (hits.empty()) {
      return false;
    }
    const project::SharedPathList list = candidates ? candidates() : nullptr;
    if (!list) {
      return false;
    }
    const auto candidate = std::find(list->begin(), list->end(), relative_path);
    if (candidate == list->end()) {
      return false;
    }
    file_index = static_cast<std::size_t>(candidate - list->begin());
    for (project::ProjectSearchResult& hit : hits) {
      hit.file_index = *file_index;
    }
    first = std::lower_bound(
        results.begin(), results.end(), *file_index,
        [](const project::ProjectSearchResult& hit, std::size_t index) { return hit.file_index < index; });
  }
  const auto last = std::find_if(first, results.end(), [&](const project::ProjectSearchResult& hit) {
    return hit.file_index != *file_index;
  });
  const bool unchanged =
      std::equal(first, last, hits.begin(), hits.end(), [](const auto& a, const auto& b) {
        return a.line == b.line && a.column == b.column && a.preview == b.preview;
      });
  if (unchanged) {
    return false;
  }
  std::optional<std::tuple<std::size_t, std::size_t, std::size_t>> selected;
  if (search.selected_index < results.size()) {
    const auto& hit = results[search.selected_index];
    selected = std::tuple{hit.file_index, hit.line, hit.column};
  }
  const auto at = results.erase(first, last);
  results.insert(at, std::make_move_iterator(hits.begin()), std::make_move_iterator(hits.end()));
  if (results.size() > project::kMaxProjectSearchResults) {
    results.resize(project::kMaxProjectSearchResults);
    search.truncated = true;
  }
  if (selected.has_value()) {
    const auto& [selected_file, selected_line, selected_column] = *selected;
    const auto same = std::find_if(results.begin(), results.end(), [&](const project::ProjectSearchResult& hit) {
      return hit.file_index == selected_file && hit.line == selected_line && hit.column == selected_column;
    });
    if (same != results.end()) {
      search.selected_index = static_cast<std::size_t>(same - results.begin());
    }
  }
  search.selected_index = results.empty() ? 0 : std::min(search.selected_index, results.size() - 1);
  ++search.results_revision;
  return true;
}

ProjectSearchCandidates SearchCandidatesFrom(const project::FileIndex& index, const ProjectSearchState& search) {
  return [&index, &search]() {
    return index
        .SnapshotPathsWithVersion(search.run_options.show_hidden ? project::ProjectFileScanMode::IncludeHidden
                                                                 : project::ProjectFileScanMode::ExcludeHidden)
        .files;
  };
}

std::optional<LiveHitsRequest> LiveHitsRequestIfStale(ProjectSearchState& search,
                                                      const std::filesystem::path& root,
                                                      const editor::TextViewport& viewport) {
  if (search.live_hits_document == viewport.document_identity() &&
      search.live_hits_content_revision == viewport.content_revision()) {
    return std::nullopt;
  }
  search.live_hits_document = viewport.document_identity();
  search.live_hits_content_revision = viewport.content_revision();
  return PrepareLiveHitsRequest(search, root, viewport);
}

}
