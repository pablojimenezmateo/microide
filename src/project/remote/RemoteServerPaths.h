#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "project/remote/RemoteProtocol.h"

namespace microide::project::remote {

std::filesystem::path DefaultServerSocketDir(const std::filesystem::path& home);

std::string_view ServerInstallScript();
std::string_view ServerInstallDirectory();
std::filesystem::path ServerSocketPath(const std::filesystem::path& dir);
std::filesystem::path ServerLogPath(const std::filesystem::path& dir);
std::filesystem::path BrowserHelperPath(const std::filesystem::path& dir);
inline constexpr std::string_view kBrowserHelperName = "microide-browser";
std::filesystem::path EditorHelperPath(const std::filesystem::path& dir);
inline constexpr std::string_view kEditorHelperName = "microide-edit";

bool EnsureServerSocketDir(const std::filesystem::path& dir, std::string* error);

std::string MakeDaemonEpoch();

SessionSurvival ReadSessionSurvival(const std::filesystem::path& root, std::string_view user);

}
