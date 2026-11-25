#pragma once

#include <cstddef>
#include <filesystem>
#include <span>
#include <vector>

namespace microide::workspace {

struct ReviewTabRef {
  std::filesystem::path path;
  std::size_t index = 0;
  bool dirty = false;
};

struct ReviewTabPlan {
  std::vector<std::filesystem::path> to_open;
  std::vector<std::size_t> to_close;
  std::vector<std::filesystem::path> reused;
  std::vector<std::filesystem::path> kept_dirty;
};

ReviewTabPlan ComputeReviewTabPlan(std::span<const ReviewTabRef> existing,
                                   std::span<const std::filesystem::path> targets);

}
