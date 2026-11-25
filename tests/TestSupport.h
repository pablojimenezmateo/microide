#pragma once

#include <chrono>
#include <concepts>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>


namespace microide::tests {

struct TestCase {
  std::string name;
  std::function<void()> run;
};

void AddTest(std::vector<TestCase>& tests, std::string_view name, std::function<void()> run);
void Expect(bool condition, std::string_view message);
template <typename Describe>
  requires std::invocable<Describe&> &&
           std::convertible_to<std::invoke_result_t<Describe&>, std::string>
void Expect(bool condition, Describe&& describe) {
  if (!condition) {
    const std::string message = describe();
    Expect(false, message);
  }
}

bool IsSanitizerBuild();

bool MachineIsOversubscribed();

class TimingWindow {
 public:
  TimingWindow();
  std::chrono::steady_clock::duration Elapsed() const;
  double StalledFraction() const;
  bool Trustworthy() const;
  bool Within(std::chrono::milliseconds budget) const;
  bool Within(std::chrono::steady_clock::duration measured, std::chrono::milliseconds budget) const;
  std::string Describe() const;

 private:
  std::chrono::steady_clock::time_point start_;
  std::uint64_t start_stall_us_ = 0;
  bool stall_known_ = false;
};

bool WaitUntil(const std::function<bool()>& predicate,
               std::chrono::milliseconds timeout = std::chrono::seconds(2),
               std::chrono::milliseconds poll_interval = std::chrono::milliseconds(5),
               const std::function<void()>& pump = {});

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

class ScopedPluginConfigHomeEnv {
 public:
  explicit ScopedPluginConfigHomeEnv(const std::filesystem::path& config_home)
      : xdg_config_home_("XDG_CONFIG_HOME", config_home.string()) {}

 private:
  ScopedEnvVar xdg_config_home_;
};

void WritePluginInit(const std::filesystem::path& root, std::string_view directory_name,
                     std::string_view content);

class ScopedAppHomes {
 public:
  ScopedAppHomes(const std::filesystem::path& state_home,
                 const std::filesystem::path& config_home)
      : xdg_state_home_("XDG_STATE_HOME", state_home.string()),
        xdg_config_home_("XDG_CONFIG_HOME", config_home.string()) {}

 private:
  ScopedEnvVar xdg_state_home_;
  ScopedEnvVar xdg_config_home_;
};

}
