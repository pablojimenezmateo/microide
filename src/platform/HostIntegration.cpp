#include "platform/HostIntegration.h"

#include <cctype>
#include <string>
#include <system_error>
#include <utility>

#include "platform/FsOps.h"
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

constexpr std::size_t kMaxUrlBytes = 8192;

bool IsAllowedUrlScheme(std::string_view url) {
  const std::size_t colon = url.find(':');
  if (colon == std::string_view::npos || colon == 0) {
    return false;
  }
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
                                     .timeout_ms = 10000,
                                 });
  if (!result.success()) {
    return Failure(result.stderr_text.empty() ? "xdg-open failed" : result.stderr_text);
  }
  return Success();
}

constexpr std::size_t kMaxNotificationSummaryBytes = 160;
constexpr std::size_t kMaxNotificationBodyBytes = 480;

std::string NotificationText(std::string_view text, std::size_t max_bytes) {
  std::string out(text.substr(0, util::Utf8ByteBudgetPrefixLength(text, max_bytes)));
  for (char& c : out) {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7f) {
      c = ' ';
    }
  }
  return out;
}

}

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
  const std::filesystem::path normalized_directory = NormalizeAbsolutePath(directory);
  if (normalized_directory.empty()) {
    return Failure("No path was provided");
  }

  std::error_code error;
  if (!std::filesystem::exists(normalized_directory, error) || error) {
    return Failure("The path does not exist");
  }

  return LaunchDesktopOpener(normalized_directory.string());
}

std::vector<std::string> DesktopNotificationCommand(std::string_view summary, std::string_view body) {
  std::string safe_summary = NotificationText(summary, kMaxNotificationSummaryBytes);
  if (safe_summary.find_first_not_of(' ') == std::string::npos) {
    safe_summary = "MicroIDE";
  }
  std::string safe_body;
  for (const char c : NotificationText(body, kMaxNotificationBodyBytes)) {
    switch (c) {
      case '&':
        safe_body.append("&amp;");
        break;
      case '<':
        safe_body.append("&lt;");
        break;
      case '>':
        safe_body.append("&gt;");
        break;
      default:
        safe_body.push_back(c);
    }
  }
  std::vector<std::string> command = {"notify-send", "--app-name=MicroIDE", "--icon=microide",
                                      "--hint=string:desktop-entry:microide", "--",
                                      std::move(safe_summary)};
  if (!safe_body.empty()) {
    command.push_back(std::move(safe_body));
  }
  return command;
}

HostIntegrationResult ShowDesktopNotification(std::string_view summary, std::string_view body) {
  const SubprocessResult result =
      LocalProcessLauncher().Run(DesktopNotificationCommand(summary, body),
                                 SubprocessOptions{
                                     .cwd = {},
                                     .stdin_text = {},
                                     .environment_overrides = {},
                                     .capture_stdout = false,
                                     .capture_stderr = true,
                                     .silence_stderr = false,
                                     .timeout_ms = 5000,
                                 });
  if (!result.success()) {
    return Failure(result.stderr_text.empty() ? "notify-send failed" : result.stderr_text);
  }
  return Success();
}

}
