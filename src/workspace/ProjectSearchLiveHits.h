#pragma once

#include <filesystem>
#include <functional>

#include "project/FileIndex.h"

#include "editor/TextViewport.h"
#include "workspace/state/WorkspaceProjectState.h"

namespace microide::workspace {

// A finished project search's results follow the files open in editors, as VS
// Code's do: an open buffer's hits are re-derived from its text -- with the
// search's own matcher (project::ProjectSearchMatcher), so they are what a fresh
// search over that text would report -- and spliced in place of the file's old
// ones. A line inserted above a hit moves its row; an edit that makes or breaks
// a match adds or drops one; a dirty buffer is searched as it stands, not as it
// was saved.

// The files a search considers, in its order (the file index's snapshot for the
// search's hidden-file mode). Asked only when a file without hits gains one.
using ProjectSearchCandidates = std::function<project::SharedPathList()>;
// Those of `index`, for the hidden-file mode `search` ran under. `index` and
// `search` must outlive the function.
ProjectSearchCandidates SearchCandidatesFrom(const project::FileIndex& index, const ProjectSearchState& search);

// Re-derives `viewport`'s file's hits in `search`. Nothing happens while a
// search runs, for a buffer outside `root`, or for a file the search did not
// consider (not a candidate, outside the scope globs). Keeps the selection on
// the same hit when it survives. True when the results changed.
bool RederiveProjectSearchHits(ProjectSearchState& search,
                               const std::filesystem::path& root,
                               const editor::TextViewport& viewport,
                               const ProjectSearchCandidates& candidates);

// RederiveProjectSearchHits when `viewport` changed since the last time it was
// asked (its document or content revision): what the edit paths call after an
// edit, cheap enough to ask every frame.
bool RederiveProjectSearchHitsIfStale(ProjectSearchState& search,
                                      const std::filesystem::path& root,
                                      const editor::TextViewport& viewport,
                                      const ProjectSearchCandidates& candidates);

}  // namespace microide::workspace
