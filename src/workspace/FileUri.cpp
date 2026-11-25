#include "workspace/FileUri.h"

#include <cctype>

#include "util/Hex.h"
#include "util/PathMatch.h"
#include "util/StringUtil.h"

namespace microide::workspace {

namespace {

bool IsUnreservedUriByte(unsigned char ch) {
  return util::IsAsciiAlnum(static_cast<unsigned char>(ch)) != 0 || ch == '-' || ch == '_' || ch == '.' || ch == '~' || ch == '/';
}

}

std::string FileUriForPath(const std::filesystem::path& path) {
  std::filesystem::path normalized;
  const std::filesystem::path* source = &path;
  if (util::PathTextNeedsNormalizing(path.native())) {
    normalized = path.lexically_normal();
    source = &normalized;
  }
  const std::string_view raw = source->native();
  std::string encoded = "file://";
  encoded.reserve(raw.size() + 8);
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

  std::string_view rest = uri.substr(kFileScheme.size());
  const std::size_t path_start = rest.find('/');
  if (path_start == std::string_view::npos) {
    return std::nullopt;
  }
  const std::string_view authority = rest.substr(0, path_start);
  if (!authority.empty() && authority != "localhost") {
    return std::nullopt;
  }
  const std::string_view encoded = rest.substr(path_start);

  std::optional<std::string> decoded = util::PercentDecodeStrict(encoded);
  if (!decoded || decoded->empty()) {
    return std::nullopt;
  }
  if (decoded->find('\0') != std::string::npos) {
    return std::nullopt;
  }
  return std::filesystem::path(*decoded).lexically_normal();
}

}
