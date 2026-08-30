#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "editor/SingleLineEditor.h"
#include "project/FileIndex.h"

namespace microide::project {

struct FileFinderResult {
  // The project-relative path, as a string. Deliberately NOT a
  // std::filesystem::path: this struct is materialized for up to kMaxResults
  // rows on EVERY keystroke, and a path costs two more allocations per row (its
  // own string plus the component split) for a field only SelectedPath() ever
  // read — 4,134 of the 13,140 allocations the ten-keystroke
  // `file_finder_type_query` phase used to make, its single largest site.
  // SelectedPath() builds the path once, when the user picks a row.
  //
  // Not a string_view into the finder's candidate blob either: five call sites
  // outside this class invalidate that cache without clearing `results_`, so a
  // view would outlive its bytes.
  std::string path_string;
  int score = 0;
};

class FileFinder {
 public:
  void SetIndex(const FileIndex* index);
  void InvalidateIndexCache();
  void SetQuery(std::string query);
  // Project-relative paths (newest-first) shown ahead of the ranked listing while
  // the query is empty, so an freshly-opened finder leads with recent files. Only
  // entries that still exist in the index are surfaced. Ignored once the user types.
  void SetRecentRelativePaths(std::vector<std::filesystem::path> paths);
  void Refresh();
  void MoveSelection(int delta);

  const std::string& query() const { return query_.text(); }
  editor::SingleLineEditor& query_state() { return query_; }
  const editor::SingleLineEditor& query_state() const { return query_; }
  // A span over the live prefix of `results_storage_`, not the vector itself.
  // The rows are overwritten in place on every keystroke and the tail is kept
  // rather than destroyed, so the storage keeps its strings' capacity across
  // refreshes; the vector's own size would be the high-water mark, not the
  // answer. See AppendResult (TD-2026-08-06-156).
  std::span<const FileFinderResult> results() const {
    return std::span<const FileFinderResult>(results_storage_.data(), results_size_);
  }
  // Files the finder can rank over, i.e. the denominator for the overlay's
  // "<shown> of <indexed>" summary.
  std::size_t indexed_file_count() const { return cached_entries_.size(); }
  // True when the backing file index was truncated (project too large / too deep),
  // so the finder's results are drawn from only a prefix of the tree and must be
  // surfaced as incomplete rather than authoritative (TD-2026-07-17-008/033).
  bool index_truncated() const { return index_ != nullptr && index_->truncated(); }
  // The specific cause(s) of any truncation, so the UI can say *why* the list is
  // incomplete (too large / too deep / unreadable folders) rather than a bare note.
  ProjectFileScanStatus index_scan_status() const {
    return index_ != nullptr ? index_->scan_status() : ProjectFileScanStatus{};
  }
  std::size_t selected_index() const { return selected_index_; }
  std::optional<std::filesystem::path> SelectedPath() const;

 private:
  // One per indexed file, rebuilt whole whenever the index version moves. Every
  // heap allocation in here is paid per file in the project, on the shell thread,
  // at the next Refresh() after the index version moves — so the entry owns NO
  // strings at all: the path bytes and their case-folded form live back to back
  // in two blobs (`path_blob_`, `lower_blob_`) and the entry is offsets into them.
  //
  // The history is the argument. This carried the same path four times (a
  // std::filesystem::path, its string, the folded string, and the folded
  // filename), then twice, and two strings per entry still meant 20,000
  // allocations to index 10,000 files on top of the 10,000 the index snapshot
  // copy cost (TD-2026-08-06-154). Two blobs make the whole rebuild a handful of
  // geometric growths, whose capacity the NEXT rebuild reuses — and they put the
  // bytes the per-keystroke scan walks in contiguous memory instead of 20,000
  // separate heap nodes.
  //
  // 32-bit offsets bound the blobs at 4 GiB. The index truncates long before
  // that (a 4 GiB path blob is ~40 million paths), and BuildCacheEntry stops
  // adding entries rather than wrapping if it is ever reached.
  struct CachedFileEntry {
    std::uint32_t path_offset = 0;
    std::uint32_t path_size = 0;
    std::uint32_t lower_offset = 0;
    std::uint32_t lower_size = 0;
    // The folded filename is a suffix of the folded path, so it is an offset, not
    // a separate string. Path separators are ASCII and case folding never adds or
    // removes one, so the last separator in the FOLDED path bounds the same
    // component it bounds in the original -- true even for a fold that changes a
    // component's byte length.
    std::uint32_t lower_filename_offset = 0;
    // Directory components ahead of the filename. Precomputed because it is a
    // ranking term (a shallower path wins an otherwise-equal match) and counting
    // separators per candidate per keystroke is a scan the build can do once.
    std::uint16_t path_segments = 0;
    // True when the case fold preserved byte offsets, i.e. `lower` and the
    // original path align byte for byte. Only then can the scorer read the
    // ORIGINAL bytes at a folded offset, which is what makes camelCase humps
    // visible ("fbc" -> FooBarConfig). A fold that changes a component's byte
    // length (U+0130) turns the bonus off for that entry rather than reading the
    // wrong byte.
    bool fold_preserves_offsets = false;
    // Presence bitmask over the folded bytes (see CharPresenceMask). A query
    // whose mask is not a subset of these cannot possibly be a subsequence, so
    // the O(len * query) scan below is skipped outright.
    std::uint64_t lower_path_mask = 0;
    std::uint64_t lower_filename_mask = 0;
  };

  // A candidate that matched, as an index into `cached_entries_` plus its score.
  // Ranking works on these rather than on materialized rows: only the visible,
  // capped prefix is ever deep-copied into `results_storage_`.
  struct RankedRef {
    std::size_t index;
    int score;
  };

  std::string_view PathView(const CachedFileEntry& entry) const {
    return std::string_view(path_blob_).substr(entry.path_offset, entry.path_size);
  }
  std::string_view LowerPathView(const CachedFileEntry& entry) const {
    return std::string_view(lower_blob_).substr(entry.lower_offset, entry.lower_size);
  }
  std::string_view LowerFilenameView(const CachedFileEntry& entry) const {
    return std::string_view(lower_blob_)
        .substr(entry.lower_offset + entry.lower_filename_offset,
                entry.lower_size - entry.lower_filename_offset);
  }

  // 64-bucket presence set: bit (byte % 64) for every byte of `text`. Subsequence
  // matching requires every query byte to APPEAR in the candidate, so
  // `(candidate & query) != query` is a sound (never false-negative) rejection.
  // Buckets collide, so it can pass an impossible candidate — the real scan then
  // rejects it. Cheap enough to run over the whole index per keystroke.
  static std::uint64_t CharPresenceMask(std::string_view text);

  // Match `query` (already folded) against `text` (folded) and return a penalty:
  // lower is better, `kNoMatch` when the query is not a subsequence at all.
  // `original` is the pre-fold bytes when they align byte for byte with `text`,
  // empty otherwise — see CachedFileEntry::fold_preserves_offsets.
  static int MatchPenalty(std::string_view text, std::string_view original,
                          std::string_view query);
  int RankMatchCached(const CachedFileEntry& entry, const std::string& query,
                      std::uint64_t query_mask) const;
  // Append one indexed path to the blobs and push its entry. Returns false when
  // the blob bound is reached, which stops the rebuild instead of truncating an
  // offset.
  bool AppendCacheEntry(std::string_view relative_path);
  void EnsureCacheBuilt();
  // Write one result row, reusing the string already sitting at that slot.
  //
  // Ranking is allocation-free (it walks views into the candidate blob) and then
  // this used to `clear()` and `push_back` up to 512 rows, so every keystroke
  // freed 512 strings and allocated 512 more to show about twenty of them —
  // ~490 allocations per keystroke, the last of `file_finder_type_query`'s
  // original 13,140 (TD-2026-08-06-156). `assign` into the retained string is
  // the same fix that took the Settings and Breakpoints rebuilds apart
  // (TD-2026-08-06-159), and it is why the tail past `results_size_` is kept
  // rather than resized away: a backspace grows the list back.
  void AppendResult(std::string_view path, int score);

  const FileIndex* index_ = nullptr;
  editor::SingleLineEditor query_;
  std::vector<std::filesystem::path> recent_relative_paths_;
  // High-water storage. `results_size_` is how many of its rows are live; the
  // rest keep their strings' capacity for the next refresh.
  std::vector<FileFinderResult> results_storage_;
  std::size_t results_size_ = 0;
  std::vector<CachedFileEntry> cached_entries_;
  // The candidate bytes, packed. Cleared (capacity retained) and refilled by a
  // rebuild, so a steady-state finder over a project whose index keeps moving
  // allocates nothing at all for them after the first build.
  std::string path_blob_;
  std::string lower_blob_;
  bool cache_ready_ = false;
  std::uint64_t cached_index_version_ = 0;
  std::size_t selected_index_ = 0;

  // Forward-typing narrowing: when the new query extends the previous one,
  // subsequence matching is monotone, so the new matches are a subset of the
  // previous matches. Re-rank only those indices instead of the whole index.
  // (Scores depend on query length, so the narrowed set is re-scored, not
  // reused.) Reset whenever the cache rebuilds or the query is empty/shrinks.
  std::string last_lower_query_;
  std::uint64_t last_match_version_ = 0;
  bool has_last_match_ = false;
  std::vector<std::size_t> last_matched_indices_;

  // Per-refresh scratch, held so a keystroke reuses the capacity the last one
  // grew instead of doubling its way back to it. On a 10,000-file project a
  // one-character query matches nearly everything, so these were rebuilt to
  // ~10,000 elements from empty on EVERY keystroke, on the shell thread: ~1.9 MB
  // of allocation and the same again in memcpy per keystroke burst, all of it
  // freed at the end of the call that grew it.
  //
  // `matched_indices_scratch_` is SWAPPED with `last_matched_indices_` at the end
  // of a refresh rather than moved into it -- a move would hand the buffer away
  // and leave the scratch empty again, which is the thing this is here to stop.
  // The swap also keeps the two distinct objects, which the narrowing path needs:
  // it reads `last_matched_indices_` while filling the scratch.
  std::vector<RankedRef> ranked_refs_scratch_;
  std::vector<std::size_t> matched_indices_scratch_;
};

}  // namespace microide::project
