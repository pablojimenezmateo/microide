#pragma once

#include <filesystem>
#include <string>

namespace microide::editor {

// Canonical normalized lookup key for the per-file presentation stores
// (diagnostics, plugin decorations, plugin surfaces). All of those maps are
// keyed by this exact string, and editor documents cache it once per path so
// the per-frame render lookups can pass a precomputed key and avoid
// re-normalizing (and re-allocating) on every frame.
inline std::string NormalizedPathKey(const std::filesystem::path& path) {
  return path.empty() ? std::string{} : path.lexically_normal().generic_string();
}

}  // namespace microide::editor
