#include "platform/HostIntegration.h"

#include <SDL3/SDL.h>

#include <cctype>
#include <string>
#include <system_error>
#include <utility>

#include "platform/Subprocess.h"

namespace microide::platform {

namespace {

HostIntegrationResult Success() {
  return HostIntegrationResult{
      .ok = true,
      .error_message = {},
  };
}

HostIntegrationResult Failure(std::string message) {
  return HostIntegrationResult{
      .ok = false,
      .error_message = std::move(message),
  };
}

std::filesystem::path NormalizeExistingPath(const std::filesystem::path& path) {
  std::error_code error;
  const std::filesystem::path absolute = std::filesystem::absolute(path, error);
  return (error ? path : absolute).lexically_normal();
}

// TD-2026-07-17-027: opening an external URL is a trust boundary — plugin- or
// project-supplied URLs must not launch arbitrary schemes (`javascript:`,
// `file:`, `data:`, custom handlers). Allowlist the schemes we intend to hand to
// the OS opener. A URL longer than this is almost certainly hostile/malformed and
// would also degrade prompt/log layout.
constexpr std::size_t kMaxUrlBytes = 8192;

bool IsAllowedUrlScheme(std::string_view url) {
  const std::size_t colon = url.find(':');
  if (colon == std::string_view::npos || colon == 0) {
    return false;  // no scheme delimiter -> not a fully-qualified URL we will open
  }
  // RFC 3986 scheme chars only, folded to lowercase for comparison.
  std::string scheme;
  scheme.reserve(colon);
  for (std::size_t i = 0; i < colon; ++i) {
    const unsigned char ch = static_cast<unsigned char>(url[i]);
    if (!(std::isalnum(ch) || ch == '+' || ch == '-' || ch == '.')) {
      return false;
    }
    scheme.push_back(static_cast<char>(std::tolower(ch)));
  }
  return scheme == "http" || scheme == "https" || scheme == "mailto";
}

}  // namespace

HostIntegrationResult OpenUrl(std::string_view url) {
  if (url.empty()) {
    return Failure("No URL was provided");
  }
  if (url.size() > kMaxUrlBytes) {
    return Failure("URL is too long to open");
  }
  if (!IsAllowedUrlScheme(url)) {
    return Failure("Refusing to open a URL with an unsupported scheme (only http, https, mailto)");
  }
  if (SDL_OpenURL(std::string(url).c_str())) {
    return Success();
  }
  return Failure(SDL_GetError());
}

HostIntegrationResult OpenPathInFileManager(const std::filesystem::path& directory) {
  const std::filesystem::path normalized_directory = NormalizeExistingPath(directory);
  if (normalized_directory.empty()) {
    return Failure("No path was provided");
  }

  std::error_code error;
  if (!std::filesystem::exists(normalized_directory, error) || error) {
    return Failure("The path does not exist");
  }

  const SubprocessResult result = RunSubprocess({"xdg-open", normalized_directory.string()},
                                                SubprocessOptions{
                                                    .cwd = {},
                                                    .stdin_text = {},
                                                    .environment_overrides = {},
                                                    .capture_stdout = false,
                                                    .capture_stderr = true,
                                                    .silence_stderr = false,
                                                    // xdg-open normally forks and returns immediately;
                                                    // a finite timeout bounds a wedged file manager so
                                                    // it can never hang the calling (UI) thread with the
                                                    // default 0 = wait-indefinitely.
                                                    .timeout_ms = 10000,
                                                });
  if (!result.success()) {
    return Failure(result.stderr_text.empty() ? "xdg-open failed" : result.stderr_text);
  }
  return Success();
}

}  // namespace microide::platform
