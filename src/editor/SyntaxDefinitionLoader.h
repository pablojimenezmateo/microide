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
  std::unordered_map<std::string, Entry> cache_;
};

}
