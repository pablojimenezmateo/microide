#include "platform/HostIntegration.h"

#include <SDL3/SDL.h>

#include <system_error>
#include <utility>

#include "platform/Subprocess.h"

#if defined(_WIN32)
#include <shellapi.h>
#include <windows.h>
#endif

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

#if defined(_WIN32)
std::wstring ToWideString(const std::filesystem::path& path) {
  return path.wstring();
}
#endif

}  // namespace

HostIntegrationResult OpenUrl(std::string_view url) {
  if (url.empty()) {
    return Failure("No URL was provided");
  }

#if defined(_WIN32)
  const std::wstring wide_url(url.begin(), url.end());
  const auto result = reinterpret_cast<std::intptr_t>(
      ShellExecuteW(nullptr, L"open", wide_url.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
  if (result <= 32) {
    return Failure("Windows failed to open the URL");
  }
  return Success();
#else
  if (SDL_OpenURL(std::string(url).c_str())) {
    return Success();
  }
  return Failure(SDL_GetError());
#endif
}

HostIntegrationResult RevealPath(const std::filesystem::path& path) {
  const std::filesystem::path normalized_path = NormalizeExistingPath(path);
  if (normalized_path.empty()) {
    return Failure("No path was provided");
  }

  std::error_code error;
  if (!std::filesystem::exists(normalized_path, error) || error) {
    return Failure("The path does not exist");
  }

#if defined(_WIN32)
  const std::wstring explorer_args = L"/select," + ToWideString(normalized_path);
  const auto result = reinterpret_cast<std::intptr_t>(
      ShellExecuteW(nullptr, L"open", L"explorer.exe", explorer_args.c_str(), nullptr, SW_SHOWNORMAL));
  if (result <= 32) {
    return Failure("Windows failed to reveal the path");
  }
  return Success();
#elif defined(__APPLE__)
  const SubprocessResult result = RunSubprocess({"open", "-R", normalized_path.string()},
                                                SubprocessOptions{
                                                    .cwd = {},
                                                    .stdin_text = {},
                                                    .environment_overrides = {},
                                                    .capture_stdout = false,
                                                    .capture_stderr = true,
                                                    .silence_stderr = false,
                                                });
  if (!result.success()) {
    return Failure(result.stderr_text.empty() ? "open -R failed" : result.stderr_text);
  }
  return Success();
#else
  const std::filesystem::path target =
      std::filesystem::is_directory(normalized_path, error) && !error
          ? normalized_path
          : (normalized_path.parent_path().empty() ? normalized_path
                                                   : normalized_path.parent_path());
  const SubprocessResult result = RunSubprocess({"xdg-open", target.string()},
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
#endif
}

}  // namespace microide::platform
