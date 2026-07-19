#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace microide::workspace {

// One side of a non-git ("plain") comparison. The caller resolves `content`
// (file read, buffer serialize, or clipboard read); `path` is the real on-disk
// file backing this side, empty for a clipboard or untitled-buffer side;
// `editable` requests that — when this is the right (primary) side and it is a
// real file — the compare's right pane save back to `path`.
struct CompareInput {
  std::string content;
  std::string label;
  std::filesystem::path path;
  bool editable = false;
};

// Reads `path` into a CompareInput for a plain comparison. Returns nullopt when
// the file is unreadable, binary, or too large (a missing file yields empty
// content — a legitimate "deleted" side). The label is the filename.
std::optional<CompareInput> ReadFileCompareInput(const std::filesystem::path& path, bool editable);

}  // namespace microide::workspace
