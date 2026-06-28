#include "workspace/FileUri.h"

#include <cctype>

#include "util/Hex.h"

namespace microide::workspace {

namespace {

bool IsUnreservedUriByte(unsigned char ch) {
  return std::isalnum(ch) != 0 || ch == '-' || ch == '_' || ch == '.' || ch == '~' || ch == '/';
}

}  // namespace

std::string FileUriForPath(const std::filesystem::path& path) {
  const std::string raw = path.lexically_normal().generic_string();
  std::string encoded = "file://";
  encoded.reserve(raw.size() + 8);
#ifdef _WIN32
  if (!raw.empty() && raw.front() != '/') {
    encoded.push_back('/');
  }
#endif
  for (unsigned char ch : raw) {
    if (IsUnreservedUriByte(ch)) {
      encoded.push_back(static_cast<char>(ch));
      continue;
    }
    encoded.push_back('%');
    util::AppendHexByte(encoded, ch);
  }
  return encoded;
}

std::optional<std::filesystem::path> PathFromFileUri(std::string_view uri) {
  static constexpr std::string_view kFileScheme = "file://";
  if (!uri.starts_with(kFileScheme)) {
    return std::nullopt;
  }

  std::string_view encoded = uri.substr(kFileScheme.size());
  if (encoded.starts_with("localhost/")) {
    encoded.remove_prefix(std::string_view("localhost").size());
  }

  std::string decoded = util::PercentDecode(encoded);
  if (decoded.empty()) {
    return std::nullopt;
  }
#ifdef _WIN32
  if (decoded.size() >= 3 && decoded[0] == '/' && std::isalpha(decoded[1]) != 0 &&
      decoded[2] == ':') {
    decoded.erase(decoded.begin());
  }
#endif
  return std::filesystem::path(decoded).lexically_normal();
}

}  // namespace microide::workspace
