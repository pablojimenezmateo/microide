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

// The mirror's object store (dev-docs/design/remote-projects.md § 6.2): file
// contents by blake3 hash under `meta/objects/`, so bytes the mirror has held once
// never cross the link again — a branch switch back, an agent restoring a file, a
// rename. Written after every verified pull and successful push; read back only
// after its hash is checked again (the store is a cache, never a source of truth).
// Bounded: Sweep removes what no path needs, oldest first, down to a budget.
class MirrorObjects {
 public:
  // Objects larger than this are not stored: they stream to disk on pull and
  // would crowd the budget.
  static constexpr std::uint64_t kMaxObjectBytes = 8u * 1024 * 1024;

  explicit MirrorObjects(std::filesystem::path directory) : directory_(std::move(directory)) {}

  void Put(const util::ContentHash& hash, std::string_view bytes);
  // The bytes, when held AND they still hash to `hash`; a damaged object is removed.
  std::optional<std::string> Get(const util::ContentHash& hash);
  bool Has(const util::ContentHash& hash) const;

  struct SweepResult {
    std::uint64_t kept_bytes = 0;
    std::uint64_t removed_bytes = 0;
    std::size_t removed = 0;
  };
  // Remove objects not in `reachable`, least recently written first, until what is
  // left fits `budget_bytes`. Reachable objects are never removed, whatever the
  // budget: dropping one costs a transfer later, keeping it costs only disk.
  SweepResult Sweep(const std::unordered_set<std::string>& reachable_hex, std::uint64_t budget_bytes);

  std::filesystem::path PathOf(const util::ContentHash& hash) const;

 private:
  std::filesystem::path directory_;
  std::mutex mutex_;  // Put/Sweep against each other; reads need none
};

}  // namespace microide::project::remote
