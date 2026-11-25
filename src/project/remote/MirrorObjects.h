#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>

#include "util/ContentHash.h"

namespace microide::project::remote {

class MirrorObjects {
 public:
  static constexpr std::uint64_t kMaxObjectBytes = 8u * 1024 * 1024;

  explicit MirrorObjects(std::filesystem::path directory) : directory_(std::move(directory)) {}

  void Put(const util::ContentHash& hash, std::string_view bytes);
  std::optional<std::string> Get(const util::ContentHash& hash);
  bool Has(const util::ContentHash& hash) const;

  struct SweepResult {
    std::uint64_t kept_bytes = 0;
    std::uint64_t removed_bytes = 0;
    std::size_t removed = 0;
  };
  SweepResult Sweep(const std::unordered_set<std::string>& reachable_hex, std::uint64_t budget_bytes);

  std::filesystem::path PathOf(const util::ContentHash& hash) const;

 private:
  std::filesystem::path directory_;
  std::mutex mutex_;
};

}
