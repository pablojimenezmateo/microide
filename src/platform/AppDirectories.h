#pragma once

#include <filesystem>
#include <string_view>

namespace microide::platform {

enum class UserDirectoryKind {
  Config,
  State,
  Data,
  Cache,
};

// The environment variable `name` as a path; empty when unset or empty. Plain
// getenv, not SDL's wrapper: it reads the live environment, so a setenv() from a
// test's ScopedEnvVar is visible (SDL's cached copy was not).
std::filesystem::path EnvironmentPath(const char* name);

std::filesystem::path ResolveUserHomeDirectory();
std::filesystem::path ResolveUserDirectory(UserDirectoryKind kind);
std::filesystem::path ResolveAppDirectory(UserDirectoryKind kind, std::string_view app_name);

}  // namespace microide::platform
