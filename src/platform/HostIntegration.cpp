#include "platform/HostIntegration.h"

#include <SDL3/SDL.h>

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

}  // namespace

HostIntegrationResult OpenUrl(std::string_view url) {
  if (url.empty()) {
    return Failure("No URL was provided");
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
                                                });
  if (!result.success()) {
    return Failure(result.stderr_text.empty() ? "xdg-open failed" : result.stderr_text);
  }
  return Success();
}

}  // namespace microide::platform
