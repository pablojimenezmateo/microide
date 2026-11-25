#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "util/ContentHash.h"

namespace microide::project::remote {

class FileVersionHistory {
 public:
  struct Version {
    util::ContentHash hash;
    std::uint64_t replaced_at = 0;

    bool operator==(const Version&) const = default;
  };

  static constexpr std::size_t kMaxVersionsPerPath = 16;
  static constexpr std::size_t kMaxPaths = 2000;
  static constexpr std::size_t kMaxPathBytes = 4096;

  void Record(std::string_view path, const util::ContentHash& hash, std::uint64_t replaced_at);
  std::vector<Version> VersionsOf(std::string_view path) const;
  bool empty() const { return versions_.empty(); }
  std::size_t path_count() const { return versions_.size(); }

  std::string Encode() const;
  static FileVersionHistory Decode(std::string_view bytes);

 private:
  void EvictOldestPath();

  std::map<std::string, std::vector<Version>, std::less<>> versions_;
};

}
