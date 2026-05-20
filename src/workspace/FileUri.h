#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace microide::workspace {

// Encode a filesystem path as a `file://` URI per RFC 3986's unreserved set.
// On Windows, a leading slash is inserted before drive-letter paths.
std::string FileUriForPath(const std::filesystem::path& path);

// Decode a `file://` URI back into a filesystem path. Returns nullopt when the
// scheme is missing or the decoded body is empty.
std::optional<std::filesystem::path> PathFromFileUri(std::string_view uri);

}  // namespace microide::workspace
