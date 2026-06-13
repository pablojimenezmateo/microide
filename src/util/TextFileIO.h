#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace microide::util {

std::optional<std::string> ReadTextFile(const std::filesystem::path& path);
bool WriteTextFileAtomically(const std::filesystem::path& path, std::string_view text);

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
// false if the file cannot be opened/read or if it contains a NUL byte (treated
// as binary). On a false return `out`'s contents are unspecified.
bool ReadFileForTextSearch(const std::filesystem::path& path, std::string& out);

}  // namespace microide::util
