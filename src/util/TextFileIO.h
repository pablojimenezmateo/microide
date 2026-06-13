#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace microide::util {

std::optional<std::string> ReadTextFile(const std::filesystem::path& path);
bool WriteTextFileAtomically(const std::filesystem::path& path, std::string_view text);

// Reads the whole file at `path` into `out`, reusing `out`'s capacity so callers
// in hot loops (project search, replace-all) avoid per-file allocation. Returns
// false if the file cannot be opened/read or if it contains a NUL byte (treated
// as binary). On a false return `out`'s contents are unspecified.
bool ReadFileForTextSearch(const std::filesystem::path& path, std::string& out);

}  // namespace microide::util
