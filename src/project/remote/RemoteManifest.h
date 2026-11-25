#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "util/ContentHash.h"

namespace microide::project::remote {

enum class ManifestEntryKind : std::uint8_t {
  File = 0,
  Symlink = 1,
  Submodule = 2,
};

struct ManifestRow {
  std::string path;
  ManifestEntryKind kind = ManifestEntryKind::File;
  std::uint64_t size = 0;
  std::uint32_t mode = 0;
  std::int64_t mtime_ns = 0;
  util::ContentHash hash;
  std::string link_target;

  friend bool operator==(const ManifestRow&, const ManifestRow&) = default;
};

inline constexpr std::size_t kMaxManifestPathBytes = 4096;

inline constexpr std::int64_t kRacySignatureWindowNs = 2'000'000'000;
std::int64_t WallClockNowNs();

bool IsSafeRelativePath(std::string_view path);

void EncodeManifestRows(const ManifestRow* rows, std::size_t count, std::string& out);
inline void EncodeManifestRows(const std::vector<ManifestRow>& rows, std::string& out) {
  EncodeManifestRows(rows.data(), rows.size(), out);
}

bool DecodeManifestRows(std::string_view bytes, std::vector<ManifestRow>& rows);

void DiffManifests(const std::vector<ManifestRow>& before, const std::vector<ManifestRow>& after,
                   std::vector<const ManifestRow*>& changed, std::vector<const std::string*>& deleted);

void EncodeWatchDelta(const std::string* const* deleted, std::size_t deleted_count,
                      const ManifestRow* const* rows, std::size_t row_count, std::string& out);
bool DecodeWatchDelta(std::string_view bytes, std::vector<std::string>& deleted,
                      std::vector<ManifestRow>& rows);

}
