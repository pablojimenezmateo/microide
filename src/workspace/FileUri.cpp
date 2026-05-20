#include "workspace/FileUri.h"

#include <cctype>
#include <iomanip>
#include <sstream>

namespace microide::workspace {

namespace {

bool IsUnreservedUriByte(unsigned char ch) {
  return std::isalnum(ch) != 0 || ch == '-' || ch == '_' || ch == '.' || ch == '~' || ch == '/';
}

}  // namespace

std::string FileUriForPath(const std::filesystem::path& path) {
  const std::string raw = path.lexically_normal().generic_string();
  std::ostringstream encoded;
  encoded << "file://";
#ifdef _WIN32
  if (!raw.empty() && raw.front() != '/') {
    encoded << '/';
  }
#endif
  for (unsigned char ch : raw) {
    if (IsUnreservedUriByte(ch)) {
      encoded << static_cast<char>(ch);
      continue;
    }
    encoded << '%' << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(ch) << std::nouppercase << std::dec;
  }
  return encoded.str();
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

  std::string decoded;
  decoded.reserve(encoded.size());
  for (std::size_t i = 0; i < encoded.size(); ++i) {
    if (encoded[i] == '%' && i + 2 < encoded.size()) {
      const auto hex_value = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
        if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
        return -1;
      };
      const int hi = hex_value(encoded[i + 1]);
      const int lo = hex_value(encoded[i + 2]);
      if (hi >= 0 && lo >= 0) {
        decoded.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    decoded.push_back(encoded[i]);
  }
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
