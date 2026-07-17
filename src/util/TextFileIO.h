#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace microide::util {

// Upper bound on a single text file we will slurp into memory. The whole file is
// buffered at once (and the CRLF path transiently doubles it), so without a cap a
// multi-gigabyte or crafted-sparse file forces one enormous allocation and an
// uncaught std::bad_alloc / OOM. Files above this are refused with a defined
// failure (nullopt / false) rather than crashing the process. 512 MiB is far
// above any realistic source file while still bounding the allocation.
inline constexpr std::uintmax_t kMaxTextFileBytes = 512ull * 1024 * 1024;

// Tighter cap for project text search. Search runs N worker threads, each buffering
// a whole candidate file at once, so the 512 MiB whole-file cap lets worst-case
// transient use reach N x 512 MiB. Line-scanning a file that large (a generated /
// minified blob) is also pure latency with no useful result. 32 MiB bounds both the
// aggregate memory and the per-file scan cost while covering every realistic source
// file; files above it are simply skipped by the searcher.
inline constexpr std::uintmax_t kMaxSearchFileBytes = 32ull * 1024 * 1024;

std::optional<std::string> ReadTextFile(const std::filesystem::path& path);
bool WriteTextFileAtomically(const std::filesystem::path& path, std::string_view text);

// Outcome of a classifying text read. Distinguishes a genuinely absent file from
// one that exists but cannot be represented as ordinary text. Callers that would
// otherwise collapse every read failure to "" (e.g. a compare tab's working-tree
// side) must branch on this so an unreadable, binary, or oversized file surfaces
// an error state instead of masquerading as an empty / whole-file-deleted document.
enum class TextFileReadStatus {
  Ok,          // read succeeded and the content is text (no embedded NUL)
  Missing,     // file does not exist (a legitimate "deleted" state -> empty is fine)
  Unreadable,  // file exists but could not be opened/read (permissions, I/O error)
  Binary,      // file contains a NUL byte -> binary, must not render as text
  TooLarge,    // file exceeds kMaxTextFileBytes and was refused before allocation
};

struct TextFileReadResult {
  TextFileReadStatus status = TextFileReadStatus::Missing;
  std::string content;  // populated only when status == Ok

  bool ok() const { return status == TextFileReadStatus::Ok; }
  bool missing() const { return status == TextFileReadStatus::Missing; }
  // True when the file exists but cannot be treated as ordinary text. These callers
  // must refuse rather than substitute empty content.
  bool is_error() const {
    return status == TextFileReadStatus::Unreadable || status == TextFileReadStatus::Binary ||
           status == TextFileReadStatus::TooLarge;
  }
};

// Classifying counterpart to ReadTextFile. Unlike ReadTextFile (which returns
// nullopt for both an absent and an unreadable file, and returns raw bytes for a
// binary file), this reports exactly which case occurred so the caller can map
// only true absence to empty content.
TextFileReadResult ReadTextFileClassified(const std::filesystem::path& path);

// Cheap on-disk identity of a file: modification time + size. Used to detect that
// a file changed underneath an open editor buffer without reading its contents.
// `exists == false, error == false` means the file is absent; `error == true`
// means the stat itself failed (treat conservatively as "unknown / changed").
//
// mtime+size is deliberately chosen over a content hash for speed/low-CPU: any
// normal external writer bumps the mtime. The only blind spot is a rewrite that
// preserves byte length AND lands within the same filesystem mtime tick, which is
// acceptable given the project's speed-first priority.
struct FileSignature {
  bool exists = false;
  bool error = false;
  std::uint64_t mtime_ticks = 0;
  std::uintmax_t size = 0;

  // Two existing files with identical mtime+size are treated as the same content.
  bool SameContentAs(const FileSignature& other) const {
    return exists && other.exists && !error && !other.error &&
           mtime_ticks == other.mtime_ticks && size == other.size;
  }
};

FileSignature StatFileSignature(const std::filesystem::path& path);

// Reads the whole file at `path` into `out`, reusing `out`'s capacity so callers
// in hot loops (project search, replace-all) avoid per-file allocation. Returns
// false if the file cannot be opened/read, if it contains a NUL byte (treated as
// binary), or if it exceeds `max_bytes` (skipped before allocation). On a false
// return `out`'s contents are unspecified. `max_bytes` defaults to the search cap;
// pass `kMaxTextFileBytes` for whole-file callers that must slurp large files.
bool ReadFileForTextSearch(const std::filesystem::path& path, std::string& out,
                           std::uintmax_t max_bytes = kMaxSearchFileBytes);

// Test seam: total ReadFileForTextSearch invocations since the last reset, counted
// at function entry before any early-out. Lets replace-all prove it touches only
// the matched-file subset rather than re-reading the whole project. Thread-safe
// because project-search workers call the reader from several threads at once.
std::size_t TextSearchReadCount();
void ResetTextSearchReadCount();

}  // namespace microide::util
