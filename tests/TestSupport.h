#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <SDL3/SDL.h>

#include "platform/HostPlatform.h"

namespace microide::tests {

struct TestCase {
  std::string name;
  // std::function (not a bare function pointer) so parameterized suites can
  // register per-case closures — e.g. one test per architecture rule so ctest
  // sharding runs them in parallel. Plain `void(*)()` callers still work via
  // implicit conversion.
  std::function<void()> run;
};

void AddTest(std::vector<TestCase>& tests, std::string_view name, std::function<void()> run);
void Expect(bool condition, std::string_view message);

// Poll `predicate` until it holds or `timeout` elapses, running `pump` (if set)
// before each check and once more after the deadline. Returns the final predicate
// result, and returns as soon as the condition holds — no fixed over-wait. This is
// the single canonical replacement for the copy-pasted
// `deadline = now()+timeout; while (now() < deadline) { pump; if (pred) ...; sleep }`
// polling loops that were duplicated across the test suite (known-tech-debt item 088).
// `pump` typically drains a mailbox / callback queue (e.g. DrainCallbacks,
// ConsumeProjectSearchUpdates) so the awaited state can advance between checks.
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

class ScopedHostPlatformOverride {
 public:
  explicit ScopedHostPlatformOverride(platform::HostPlatform platform);
  ~ScopedHostPlatformOverride();

 private:
  std::optional<platform::HostPlatform> previous_;
};

void EnsureDummySdlVideoInitialized();
void ResetSdlModStateForTests();

class ScopedSdlModState {
 public:
  explicit ScopedSdlModState(SDL_Keymod modifiers);
  ~ScopedSdlModState();

  ScopedSdlModState(const ScopedSdlModState&) = delete;
  ScopedSdlModState& operator=(const ScopedSdlModState&) = delete;

 private:
  SDL_Keymod previous_mods_ = SDL_KMOD_NONE;
};

}  // namespace microide::tests
