#pragma once

#include <cstddef>
#include <cstdint>

namespace microide::tests::perf {

struct AllocationSnapshot {
  std::uint64_t allocations = 0;
  std::uint64_t frees = 0;
  std::uint64_t bytes_allocated = 0;
  std::uint64_t bytes_freed = 0;
};

struct AllocationDelta {
  std::int64_t allocations = 0;
  std::int64_t frees = 0;
  std::int64_t bytes_allocated = 0;
  std::int64_t bytes_freed = 0;
};

class Allocations {
 public:
  static AllocationSnapshot Snapshot();
  static AllocationDelta DeltaSince(const AllocationSnapshot& before);
};

}  // namespace microide::tests::perf
