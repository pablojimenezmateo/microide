#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

// True in an ASAN or TSAN build, where the code under test runs several times
// slower: for the few assertions about absolute timing that measure the machine
// rather than the code there (see kWaitTimeoutScale in TestSupport.cpp).
bool IsSanitizerBuild();

// True while the machine runs past its cores (1-minute load average above the
// hardware threads): a starved thread can halve a measured rate without inflating
// the minimum it also measured, so a throughput bound means nothing then.
bool MachineIsOversubscribed();

// A measured stretch of wall time, and how much of it the machine spent with
// runnable work waiting for a CPU (Linux pressure stall information,
// /proc/pressure/cpu "some", read at both ends). An assertion about elapsed time
// or a rate measures the machine as much as the code when the stretch it timed was
// starved -- and the 1-minute load average MachineIsOversubscribed reads lags a
// parallel ctest that started seconds earlier, which is how the throughput and
// LSP-startup tests flaked with the guard in place.
class TimingWindow {
 public:
  TimingWindow();
  std::chrono::steady_clock::duration Elapsed() const;
  // The share of the elapsed wall time with a task waiting for a CPU, 0..1; 0
  // where the kernel does not report it.
  double StalledFraction() const;
  // The machine kept time over this window: not a sanitizer build, not starved
  // (StalledFraction under 10%), not oversubscribed. Gate rate assertions on it.
  bool Trustworthy() const;
  // Elapsed() within `budget`, the budget scaled as WaitUntil's timeouts are in a
  // sanitizer build and four times again over a starved window. For "this did
  // not block" bounds, which must keep their meaning on a busy machine.
  bool Within(std::chrono::milliseconds budget) const;
  // The same for a duration the test measured itself (a stretch inside the window).
  bool Within(std::chrono::steady_clock::duration measured, std::chrono::milliseconds budget) const;
  // "took N ms (M% of it starved)", for assertion messages.
  std::string Describe() const;

 private:
  std::chrono::steady_clock::time_point start_;
  std::uint64_t start_stall_us_ = 0;
  bool stall_known_ = false;
};

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

// Points the plugin host's config lookup (XDG_CONFIG_HOME, APPDATA) at a fixture
// directory for the lifetime of the object. Six test files used to carry their
// own copy of this and of WritePluginInit.
class ScopedPluginConfigHomeEnv {
 public:
  explicit ScopedPluginConfigHomeEnv(const std::filesystem::path& config_home)
      : xdg_config_home_("XDG_CONFIG_HOME", config_home.string()),
        appdata_("APPDATA", config_home.string()) {}

 private:
  ScopedEnvVar xdg_config_home_;
  ScopedEnvVar appdata_;
};

// Write `<root>/<directory_name>/init.lua`.
void WritePluginInit(const std::filesystem::path& root, std::string_view directory_name,
                     std::string_view content);

// Scope the app config/state homes so persistence is test-local on every
// platform (XDG on Linux, *APPDATA on Windows). Two test files carried a
// byte-identical private copy of this; a persistence test that forgets one of
// the four variables writes into the developer's real profile, which is exactly
// the kind of thing a copy drifts into.
class ScopedAppHomes {
 public:
  ScopedAppHomes(const std::filesystem::path& state_home,
                 const std::filesystem::path& config_home)
      : xdg_state_home_("XDG_STATE_HOME", state_home.string()),
        xdg_config_home_("XDG_CONFIG_HOME", config_home.string()),
        localappdata_("LOCALAPPDATA", state_home.string()),
        appdata_("APPDATA", config_home.string()) {}

 private:
  ScopedEnvVar xdg_state_home_;
  ScopedEnvVar xdg_config_home_;
  ScopedEnvVar localappdata_;
  ScopedEnvVar appdata_;
};

class ScopedHostPlatformOverride {
 public:
  explicit ScopedHostPlatformOverride(platform::HostPlatform platform);
  ~ScopedHostPlatformOverride();

 private:
  std::optional<platform::HostPlatform> previous_;
};

}  // namespace microide::tests
