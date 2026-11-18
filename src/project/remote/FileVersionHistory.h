#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "util/ContentHash.h"

namespace microide::project::remote {

// Which contents a mirrored file had before the host replaced them, newest first:
// the index behind File History. It names versions only. Their bytes are in the
// mirror's object store (MirrorObjects), which holds every content the mirror
// has had until its budget pushes the oldest out -- so keeping history costs no
// disk of its own, and a version whose bytes are gone is simply not offered.
//
// What it is for: an agent on the host rewrites a file several times, or
// overwrites work that was never committed, and git has none of it.
class FileVersionHistory {
 public:
  struct Version {
    util::ContentHash hash;
    // Wall clock, seconds since the epoch, when the host replaced this content.
    std::uint64_t replaced_at = 0;

    bool operator==(const Version&) const = default;
  };

  static constexpr std::size_t kMaxVersionsPerPath = 16;
  // At most this many paths are kept; the one whose newest version is oldest goes.
  static constexpr std::size_t kMaxPaths = 2000;
  static constexpr std::size_t kMaxPathBytes = 4096;

  // `hash` was `path`'s content until `replaced_at`. A content replaced twice in
  // a row (restored, then replaced again) is listed once, at its latest time.
  void Record(std::string_view path, const util::ContentHash& hash, std::uint64_t replaced_at);
  // Newest first; empty for a path with no history.
  std::vector<Version> VersionsOf(std::string_view path) const;
  bool empty() const { return versions_.empty(); }
  std::size_t path_count() const { return versions_.size(); }

  // The file form (meta/history). Decode trusts nothing: a truncated or forged
  // file yields the records before the damage, within the bounds above.
  std::string Encode() const;
  static FileVersionHistory Decode(std::string_view bytes);

 private:
  void EvictOldestPath();

  std::map<std::string, std::vector<Version>, std::less<>> versions_;  // newest first
};

}  // namespace microide::project::remote
