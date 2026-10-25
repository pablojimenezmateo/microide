#include "workspace/ProjectSearchLiveHits.h"

#include <algorithm>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "project/GlobMatch.h"
#include "project/ProjectSearchService.h"

namespace microide::workspace {

bool RederiveProjectSearchHits(ProjectSearchState& search,
                               const std::filesystem::path& root,
                               const editor::TextViewport& viewport,
                               const ProjectSearchCandidates& candidates) {
  if (search.running || search.run_query.empty() || root.empty() || viewport.path().empty()) {
    return false;
  }
  const std::filesystem::path relative = viewport.path().lexically_normal().lexically_relative(root.lexically_normal());
  if (relative.empty() || relative.native().starts_with("..")) {
    return false;
  }
  const std::string relative_path = relative.generic_string();
  const project::GlobSet include = project::GlobSet::Parse(search.run_options.include_globs);
  const project::GlobSet exclude = project::GlobSet::Parse(search.run_options.exclude_globs);
  if ((!include.empty() && !include.Matches(relative_path)) || (!exclude.empty() && exclude.Matches(relative_path))) {
    return false;
  }
  const project::ProjectSearchMatcher matcher(search.run_query, search.run_options);
  if (!matcher.valid()) {
    return false;
  }

  // The results are sorted by (file_index, line, column): the file's hits are one
  // run of them, and they say where the file sits among the candidates.
  auto& results = search.results;
  auto first = std::find_if(results.begin(), results.end(), [&](const project::ProjectSearchResult& hit) {
    return hit.relative_path_string == relative_path;
  });
  std::optional<std::size_t> file_index;
  if (first != results.end()) {
    file_index = first->file_index;
  }
  const editor::TextBuffer& lines = viewport.lines();
  std::vector<project::ProjectSearchResult> hits = project::SearchLines(
      matcher, lines.LineCount(), [&lines](std::size_t i) { return lines.LineView(i); }, relative_path,
      file_index.value_or(0), project::kMaxProjectSearchResults);
  if (!file_index.has_value()) {
    if (hits.empty()) {
      return false;  // no hits before, none now
    }
    // A file gaining its first hit: whether the search considered it at all, and
    // where, is the candidate list's to say.
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
  // Keep the selection on its hit: the same file, line and column, else where it
  // was.
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

bool RederiveProjectSearchHitsIfStale(ProjectSearchState& search,
                                      const std::filesystem::path& root,
                                      const editor::TextViewport& viewport,
                                      const ProjectSearchCandidates& candidates) {
  if (search.live_hits_document == viewport.document_identity() &&
      search.live_hits_content_revision == viewport.content_revision()) {
    return false;
  }
  search.live_hits_document = viewport.document_identity();
  search.live_hits_content_revision = viewport.content_revision();
  return RederiveProjectSearchHits(search, root, viewport, candidates);
}

}  // namespace microide::workspace
