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

  // Split authority from path: after "file://" the authority runs up to the first
  // '/'. Only an empty authority (file:///path) or the explicit local host
  // (file://localhost/path) is a local file. A non-local authority such as
  // file://server/share is a UNC/remote reference the editor must not silently
  // treat as the relative path "server/share".
  std::string_view rest = uri.substr(kFileScheme.size());
  const std::size_t path_start = rest.find('/');
  if (path_start == std::string_view::npos) {
    return std::nullopt;  // no path component
  }
  const std::string_view authority = rest.substr(0, path_start);
  if (!authority.empty() && authority != "localhost") {
    return std::nullopt;  // non-local authority
  }
  const std::string_view encoded = rest.substr(path_start);

  // Strict percent-decode: a malformed escape (e.g. %zz or a trailing %) rejects
  // the whole URI rather than decoding to a path containing a literal '%'.
  std::optional<std::string> decoded = util::PercentDecodeStrict(encoded);
  if (!decoded || decoded->empty()) {
    return std::nullopt;
  }
  // A decoded NUL can never appear in a legitimate path and would truncate
  // downstream C-string uses; reject it.
  if (decoded->find('\0') != std::string::npos) {
    return std::nullopt;
  }
#ifdef _WIN32
  if (decoded->size() >= 3 && (*decoded)[0] == '/' && std::isalpha((*decoded)[1]) != 0 &&
      (*decoded)[2] == ':') {
    decoded->erase(decoded->begin());
  }
#endif
  return std::filesystem::path(*decoded).lexically_normal();
}

}  // namespace microide::workspace
