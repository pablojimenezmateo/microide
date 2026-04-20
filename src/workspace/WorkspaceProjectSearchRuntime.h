#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "project/ProjectSearchService.h"

namespace microide::workspace {

class WorkspaceProjectSearchRuntime {
 public:
  void Initialize();
  void Shutdown();

  bool HandlesEvent(Uint32 type) const;
  Uint32 event_type() const { return event_type_; }
  std::uint64_t active_run_id() const { return active_run_id_; }

  std::uint64_t Start(const std::filesystem::path& root,
                      std::string query,
                      project::ProjectSearchOptions options = {},
                      std::vector<std::filesystem::path> indexed_files = {});
  void Stop();
  std::optional<project::ProjectSearchUpdate> ConsumeActiveUpdate();

 private:
  project::ProjectSearchService service_;
  Uint32 event_type_ = 0;
  std::uint64_t active_run_id_ = 0;
};

}  // namespace microide::workspace
