#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "editor/RuntimeSyntaxRegistry.h"

namespace microide::editor::runtime_syntax {

std::vector<RuntimeSyntaxDefinitionData> LoadDefinitionsFromDirectories(
    const std::vector<std::filesystem::path>& directories,
    std::vector<std::string>* errors = nullptr);

// Content-based fingerprint of the syntax `.lua` sources under a set of
// directories, used to decide whether a reload can be skipped. The fingerprint
// is a pure function of the discovered files' paths and byte contents, so any
// content edit changes it. Unchanged files (matching cached mtime+size) reuse
// their previously computed content hash instead of being re-read, so a poll
// that finds nothing changed no longer re-reads every source file. Compute is
// internally synchronized (a mutex guards the cache), so it may be called from a
// background reload worker and the main thread across a worker-start transition
// (TD-2026-07-17A-108); it is not meant to run two computes truly concurrently.
class SyntaxSourceFingerprint {
 public:
  std::uint64_t Compute(const std::vector<std::filesystem::path>& directories);
  void Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.clear();
  }

 private:
  struct Entry {
    std::filesystem::file_time_type mtime{};
    std::uintmax_t size = 0;
    std::uint64_t content_hash = 0;
  };
  std::mutex mutex_;
  std::unordered_map<std::string, Entry> cache_;  // key = path.generic_string()
};

}  // namespace microide::editor::runtime_syntax
