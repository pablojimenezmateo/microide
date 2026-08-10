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

}  // namespace

std::string FileUriForPath(const std::filesystem::path& path) {
  // `lexically_normal()` costs ~12 allocations (a fresh path plus a component list
  // holding a string per component) and `generic_string()` one more, and this runs
  // three times per keystroke on the LSP sync path — once to invalidate the inlay
  // generation, once for code lenses, once to resolve the open document — for a
  // path the editor normalized when it opened the file. `PathTextNeedsNormalizing`
  // is the allocation-free scan that says so, leaving the common case at the one
  // allocation the result itself needs (TD-2026-08-06-159).
#ifdef _WIN32
  // Windows' `native()` is a wide string and its separator is not the generic
  // one, so the guard's byte scan does not apply and the conversion is real work
  // either way. Keep the original form there.
  const std::string raw_storage = path.lexically_normal().generic_string();
  const std::string_view raw = raw_storage;
#else
  std::filesystem::path normalized;
  const std::filesystem::path* source = &path;
  if (util::PathTextNeedsNormalizing(path.native())) {
    normalized = path.lexically_normal();
    source = &normalized;
  }
  const std::string_view raw = source->native();
#endif
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
  if (decoded->size() >= 3 && (*decoded)[0] == '/' &&
      util::IsAsciiAlpha(static_cast<unsigned char>((*decoded)[1])) != 0 && (*decoded)[2] == ':') {
    decoded->erase(decoded->begin());
  }
#endif
  return std::filesystem::path(*decoded).lexically_normal();
}

}  // namespace microide::workspace
