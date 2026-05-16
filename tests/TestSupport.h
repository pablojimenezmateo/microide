#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "platform/HostPlatform.h"

namespace microide::tests {

struct TestCase {
  std::string name;
  void (*run)();
};

void AddTest(std::vector<TestCase>& tests, std::string_view name, void (*run)());
void Expect(bool condition, std::string_view message);

std::filesystem::path TestRoot();
std::filesystem::path FixturePath(std::string_view relative_path);
std::string ReadFile(const std::filesystem::path& path);
void WriteFile(const std::filesystem::path& path, const std::string& content);
void CopyTree(const std::filesystem::path& source, const std::filesystem::path& destination);
std::string ShellEscape(std::string_view text);
std::string EscapedRepoPath(const std::filesystem::path& repo_path);
int RunCommand(const std::string& command);
void RequireCommandSuccess(const std::string& command, std::string_view context);
int RunGitCommand(const std::filesystem::path& repo_path, const std::vector<std::string>& args);
void RequireGitCommandSuccess(const std::filesystem::path& repo_path,
                              const std::vector<std::string>& args,
                              std::string_view context);
void InitializeGitRepo(const std::filesystem::path& repo_path);
void CommitAll(const std::filesystem::path& repo_path,
               std::string_view message,
               std::string_view context);

class TemporaryDirectory {
 public:
  TemporaryDirectory();
  ~TemporaryDirectory();

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

class ScopedEnvVar {
 public:
  ScopedEnvVar(std::string name, std::string value);
  ~ScopedEnvVar();

 private:
  void Set(const std::string& value);

  std::string name_;
  bool had_previous_ = false;
  std::string previous_value_;
};

class ScopedHostPlatformOverride {
 public:
  explicit ScopedHostPlatformOverride(platform::HostPlatform platform);
  ~ScopedHostPlatformOverride();

 private:
  std::optional<platform::HostPlatform> previous_;
};

}  // namespace microide::tests
