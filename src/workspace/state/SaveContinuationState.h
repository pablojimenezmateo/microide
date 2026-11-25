#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "workspace/state/WorkspacePromptState.h"

namespace microide::workspace {

struct SaveContinuation {
  enum class Kind {
    CloseTab,
    PathMutation,
    CloseProject,
    Quit,
  };

  std::uint64_t id = 0;
  Kind kind = Kind::CloseTab;
  std::vector<std::uint64_t> waiting_tab_ids;
  std::size_t total_tab_count = 0;
  std::filesystem::path project_root;

  std::uint64_t tab_id = 0;
  std::optional<PromptSurfaceState> path_mutation;
  std::filesystem::path return_root;
  bool switched_to_project = false;
  std::vector<std::filesystem::path> remaining_roots;
};

class SaveContinuationQueue {
 public:
  struct Settled {
    std::vector<SaveContinuation> ready;
    std::vector<SaveContinuation> cancelled;
  };

  std::uint64_t Add(SaveContinuation continuation);

  Settled Complete(std::uint64_t tab_id, bool saved);

  std::optional<SaveContinuation> Remove(std::uint64_t id);

  const SaveContinuation* Find(std::uint64_t id) const;
  bool Empty() const { return continuations_.empty(); }
  const std::vector<SaveContinuation>& All() const { return continuations_; }

 private:
  std::vector<SaveContinuation> continuations_;
  std::uint64_t next_id_ = 1;
};

}
