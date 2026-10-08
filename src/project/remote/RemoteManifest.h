#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "util/ContentHash.h"

namespace microide::project::remote {

// One row of a workspace's manifest (dev-docs/design/remote-projects.md § 6.2): a
// path in the content set and what the host holds there. Metadata is complete from
// the first round trip; content is fetched separately, by `hash`.
enum class ManifestEntryKind : std::uint8_t {
  File = 0,
  // A link inside the root, recreated as a link; `link_target` is what it points at.
  Symlink = 1,
  // A gitlink (submodule): listed, not descended (Phase 3).
  Submodule = 2,
};

struct ManifestRow {
  std::string path;  // relative to the workspace root, '/'-separated
  ManifestEntryKind kind = ManifestEntryKind::File;
  std::uint64_t size = 0;
  std::uint32_t mode = 0;  // permission bits (exec bits survive the trip)
  std::int64_t mtime_ns = 0;  // the host's clock: shown, never compared
  util::ContentHash hash;  // File only
  std::string link_target;  // Symlink only

  friend bool operator==(const ManifestRow&, const ManifestRow&) = default;
};

// Bounds on one decoded path. A host path deeper than this cannot be materialized
// under the mirror prefix anyway (§ 6.2: the engine checks the longest row).
inline constexpr std::size_t kMaxManifestPathBytes = 4096;

// Whether `path` is a relative path that stays under the root by construction: not
// empty, not absolute, no empty, "." or ".." component, no NUL, no backslash, no
// component over 255 bytes. Every path the server sends is checked with this before
// it names anything on disk (§ 6.9).
bool IsSafeRelativePath(std::string_view path);

// A chunk of rows, packed: each path is stored as the length it shares with the
// previous row's path in the same chunk plus the rest, so a sorted manifest costs
// little more than its basenames. Chunks decode independently.
void EncodeManifestRows(const ManifestRow* rows, std::size_t count, std::string& out);
inline void EncodeManifestRows(const std::vector<ManifestRow>& rows, std::string& out) {
  EncodeManifestRows(rows.data(), rows.size(), out);
}

// Appends the decoded rows to `rows`. False on any malformed byte, an unsafe path,
// an unknown kind or trailing bytes; `rows` then holds whatever came before the
// chunk (the decoder never leaves half a chunk behind).
bool DecodeManifestRows(std::string_view bytes, std::vector<ManifestRow>& rows);

// The difference between two manifests, both sorted and unique: rows of `after`
// that are new or differ from `before`, and paths of `before` that `after` lacks.
void DiffManifests(const std::vector<ManifestRow>& before, const std::vector<ManifestRow>& after,
                   std::vector<const ManifestRow*>& changed, std::vector<const std::string*>& deleted);

// A WatchDelta payload: deleted paths, then changed rows (a manifest chunk).
void EncodeWatchDelta(const std::string* const* deleted, std::size_t deleted_count,
                      const ManifestRow* const* rows, std::size_t row_count, std::string& out);
// Appends to `deleted` and `rows`; false (both untouched) on malformed bytes or an
// unsafe path.
bool DecodeWatchDelta(std::string_view bytes, std::vector<std::string>& deleted,
                      std::vector<ManifestRow>& rows);

}  // namespace microide::project::remote
