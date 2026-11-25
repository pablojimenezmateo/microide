#pragma once

#include <filesystem>
#include <functional>

#include "project/FileIndex.h"

#include "editor/TextViewport.h"
#include "workspace/state/WorkspaceProjectState.h"

namespace microide::workspace {

using ProjectSearchCandidates = std::function<project::SharedPathList()>;
ProjectSearchCandidates SearchCandidatesFrom(const project::FileIndex& index, const ProjectSearchState& search);

bool RederiveProjectSearchHits(ProjectSearchState& search,
                               const std::filesystem::path& root,
                               const editor::TextViewport& viewport,
                               const ProjectSearchCandidates& candidates);

bool RederiveProjectSearchHitsIfStale(ProjectSearchState& search,
                                      const std::filesystem::path& root,
                                      const editor::TextViewport& viewport,
                                      const ProjectSearchCandidates& candidates);

}
