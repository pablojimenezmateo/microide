#include "platform/HostIntegration.h"

#include <cctype>
#include <string>
#include <system_error>
#include <utility>

#include "platform/ProcessLauncher.h"
#include "platform/Subprocess.h"
#include "util/StringUtil.h"

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
    if (!(util::IsAsciiAlnum(static_cast<unsigned char>(ch)) || ch == '+' || ch == '-' || ch == '.')) {
      return false;
    }
    scheme.push_back(util::ToLowerAsciiChar(static_cast<char>(ch)));
  }
  return scheme == "http" || scheme == "https" || scheme == "mailto";
}

// Hand one already-validated argument to the desktop opener. Both entry points below
// used to do this differently — SDL_OpenURL for the URL, RunSubprocess("xdg-open")
// for the directory — even though SDL_OpenURL is itself a forked xdg-open on this
// platform. One path means one timeout policy and one error string, and it is what
// takes the windowing library out of the kernel.
//
// LocalProcessLauncher() explicitly, and this is one of the two places that is right:
// opening a file manager or a browser on the build server is always wrong, so this
// spawn must never follow the project (the design's "Remote: Open Local Terminal" is
// the other). Typed as such rather than left to a convention.
HostIntegrationResult LaunchDesktopOpener(std::string argument) {
  const SubprocessResult result =
      LocalProcessLauncher().Run({"xdg-open", std::move(argument)},
                                 SubprocessOptions{
                                     .cwd = {},
                                     .stdin_text = {},
                                     .environment_overrides = {},
                                     .capture_stdout = false,
                                     .capture_stderr = true,
                                     .silence_stderr = false,
                                     // xdg-open normally forks and returns at once; a
                                     // finite timeout bounds a wedged handler so it can
                                     // never hang the calling (UI) thread with the
                                     // default 0 = wait-indefinitely.
                                     .timeout_ms = 10000,
                                 });
  if (!result.success()) {
    return Failure(result.stderr_text.empty() ? "xdg-open failed" : result.stderr_text);
  }
  return Success();
}

}  // namespace

bool IsOpenableExternalUrl(std::string_view url) {
  return !url.empty() && url.size() <= kMaxUrlBytes && IsAllowedUrlScheme(url);
}

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
  return LaunchDesktopOpener(std::string(url));
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

  return LaunchDesktopOpener(normalized_directory.string());
}

}  // namespace microide::platform
