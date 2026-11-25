#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

#include "editor/HighlightPrefetch.h"
#include "project/ProjectBackgroundExecutor.h"

namespace microide::editor {

class HighlightPrefetchService {
 public:
  ~HighlightPrefetchService() { Shutdown(); }
  void SetWakeCallback(std::function<void()> wake);

  void Request(HighlightPrefetchRequest request);

  struct WindowKey {
    const void* viewport = nullptr;
    std::uint64_t content_revision = 0;
    std::uint64_t syntax_revision = 0;
    std::size_t start_line = 0;
    std::size_t count = 0;
    bool operator==(const WindowKey&) const = default;
  };
  bool Admit(const WindowKey& key) {
    if (last_admitted_.has_value() && *last_admitted_ == key) {
      return false;
    }
    last_admitted_ = key;
    return true;
  }

  void RequestCheckpoints(HighlightCheckpointRequest request);

  void PrewarmDefinition(std::uint32_t definition_id);

  void PrewarmForViewport(const void* viewport_token,
                          const std::function<std::uint32_t()>& resolve_definition_id);

  std::vector<HighlightPrefetchResult> DrainResults();

  std::vector<HighlightCheckpointResult> DrainCheckpointResults();

  void Shutdown();

 private:
  std::optional<WindowKey> last_admitted_;
  template <typename RequestT, typename ResultT, typename Compute>
  void PostLatestAndCollect(const char* key_prefix, RequestT request, Compute compute,
                            std::mutex& mutex, std::vector<ResultT>& results);

  project::ProjectBackgroundExecutor executor_;
  std::function<void()> wake_;
  std::mutex results_mutex_;
  std::vector<HighlightPrefetchResult> results_;
  std::mutex checkpoint_results_mutex_;
  std::vector<HighlightCheckpointResult> checkpoint_results_;
  const void* last_prewarm_viewport_ = nullptr;
};

}
