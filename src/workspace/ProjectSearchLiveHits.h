#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "editor/DocumentSnapshot.h"
#include "editor/TextViewport.h"
#include "project/FileIndex.h"
#include "project/ProjectSearchService.h"
#include "workspace/state/WorkspaceProjectState.h"

namespace microide::workspace {

using ProjectSearchCandidates = std::function<project::SharedPathList()>;
ProjectSearchCandidates SearchCandidatesFrom(const project::FileIndex& index, const ProjectSearchState& search);

struct LiveHitsKey {
  const void* document = nullptr;
  std::uint64_t content_revision = 0;
  std::string relative_path;
  std::string query;
  project::ProjectSearchOptions options;
};

struct LiveHitsRequest {
  LiveHitsKey key;
  editor::DocumentSnapshot text;
};

struct LiveHits {
  LiveHitsKey key;
  std::vector<project::ProjectSearchResult> hits;
};

std::optional<LiveHitsRequest> PrepareLiveHitsRequest(const ProjectSearchState& search,
                                                      const std::filesystem::path& root,
                                                      const editor::TextViewport& viewport);

LiveHits SearchLiveHits(const LiveHitsRequest& request);

bool SpliceLiveHits(ProjectSearchState& search, LiveHits live, const ProjectSearchCandidates& candidates);

std::optional<LiveHitsRequest> LiveHitsRequestIfStale(ProjectSearchState& search,
                                                      const std::filesystem::path& root,
                                                      const editor::TextViewport& viewport);

}
