#include "TestSupport.h"

#include "platform/Subprocess.h"

#include <atomic>
#include <chrono>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace microide::tests {

void AddTest(std::vector<TestCase>& tests, std::string_view name, std::function<void()> run) {
  tests.push_back(TestCase{std::string(name), std::move(run)});
}

void Expect(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

namespace {

#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
#define MICROIDE_TEST_SANITIZER_BUILD 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
#define MICROIDE_TEST_SANITIZER_BUILD 1
#endif
#endif

// A sanitizer build runs the code under test several times slower — TSAN
// instruments every memory access and serializes through its shadow state — and
// many of these waits are on a real subprocess (a mock DAP adapter, git, a
// language server) with the sharded suite running several of them at once. A
// deadline chosen for an uninstrumented run then measures the sanitizer rather
// than the product. Stretch every deadline here instead of tuning them one at a
// time — a wait returns the moment its predicate holds, so a higher ceiling costs
// a passing run nothing, and only a genuinely stuck test pays the extra seconds.
//
// This multiplier is NOT a fix for a subprocess wait, and the comment here used
// to claim it was: it named
// `DebugService/SessionResolvesStackOnStopAndStepsResume` as the case it
// resolved, and that test then timed out again behind the 4x on a loaded runner.
// Scaling a guessed budget by a constant produces a bigger guess. A wait whose
// duration is a property of the MACHINE rather than of the product needs no
// budget at all — see `PollUntil` in DebugServiceTests.cpp, which is bounded by a
// hang ceiling and the per-test watchdog instead. Reach for that shape rather
// than raising this number again.
#if defined(MICROIDE_TEST_SANITIZER_BUILD)
constexpr int kWaitTimeoutScale = 4;
#else
constexpr int kWaitTimeoutScale = 1;
#endif

}  // namespace

bool IsSanitizerBuild() { return kWaitTimeoutScale != 1; }

bool MachineIsOversubscribed() {
  double load = 0.0;
  const unsigned threads = std::max(1u, std::thread::hardware_concurrency());
  return getloadavg(&load, 1) == 1 && load > threads;
}

namespace {

// The cumulative microseconds with some task stalled waiting for a CPU, or
// nullopt where the kernel has no PSI (not Linux, or built without it).
std::optional<std::uint64_t> CpuStallTotalMicros() {
  std::ifstream pressure("/proc/pressure/cpu");
  std::string line;
  while (std::getline(pressure, line)) {
    if (!line.starts_with("some ")) {
      continue;
    }
    const std::size_t at = line.find("total=");
    if (at == std::string::npos) {
      return std::nullopt;
    }
    return std::strtoull(line.c_str() + at + 6, nullptr, 10);
  }
  return std::nullopt;
}

constexpr double kStarvedFraction = 0.10;

}  // namespace

TimingWindow::TimingWindow() : start_(std::chrono::steady_clock::now()) {
  if (const auto total = CpuStallTotalMicros(); total.has_value()) {
    start_stall_us_ = *total;
    stall_known_ = true;
  }
}

std::chrono::steady_clock::duration TimingWindow::Elapsed() const {
  return std::chrono::steady_clock::now() - start_;
}

double TimingWindow::StalledFraction() const {
  const auto total = CpuStallTotalMicros();
  const auto elapsed_us =
      std::chrono::duration_cast<std::chrono::microseconds>(Elapsed()).count();
  if (!stall_known_ || !total.has_value() || elapsed_us <= 0 || *total < start_stall_us_) {
    return 0.0;
  }
  return std::min(1.0, static_cast<double>(*total - start_stall_us_) / static_cast<double>(elapsed_us));
}

bool TimingWindow::Trustworthy() const {
  return !IsSanitizerBuild() && !MachineIsOversubscribed() && StalledFraction() < kStarvedFraction;
}

bool TimingWindow::Within(std::chrono::milliseconds budget) const {
  return Within(Elapsed(), budget);
}

bool TimingWindow::Within(std::chrono::steady_clock::duration measured,
                          std::chrono::milliseconds budget) const {
  const int starved = StalledFraction() < kStarvedFraction ? 1 : 4;
  return measured <= budget * kWaitTimeoutScale * starved;
}

std::string TimingWindow::Describe() const {
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Elapsed()).count();
  return "took " + std::to_string(ms) + " ms (" +
         std::to_string(static_cast<int>(StalledFraction() * 100.0)) + "% of it starved)";
}

bool WaitUntil(const std::function<bool()>& predicate,
               std::chrono::milliseconds timeout,
               std::chrono::milliseconds poll_interval,
               const std::function<void()>& pump) {
  const auto deadline = std::chrono::steady_clock::now() + timeout * kWaitTimeoutScale;
  while (std::chrono::steady_clock::now() < deadline) {
    if (pump) {
      pump();
    }
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(poll_interval);
  }
  // One last drain + check after the deadline: work posted just before the
  // deadline can still be pending, and every hand-rolled loop this replaces did
  // the same final check.
  if (pump) {
    pump();
  }
  return predicate();
}

std::filesystem::path TestRoot() {
  return std::filesystem::path(MICROIDE_TEST_SOURCE_DIR).lexically_normal();
}

std::filesystem::path FixturePath(std::string_view relative_path) {
  return TestRoot() / "fixtures" / relative_path;
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("failed to read file: " + path.string());
  }

  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

void WriteFile(const std::filesystem::path& path, const std::string& content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("failed to write file: " + path.string());
  }
  stream << content;
}

void CopyTree(const std::filesystem::path& source, const std::filesystem::path& destination) {
  const std::filesystem::path normalized_source = source.lexically_normal();
  const std::filesystem::path normalized_destination = destination.lexically_normal();
  std::filesystem::create_directories(normalized_destination);
  for (const auto& entry : std::filesystem::recursive_directory_iterator(normalized_source)) {
    const auto relative = entry.path().lexically_relative(normalized_source);
    const auto target = normalized_destination / relative;
    if (entry.is_directory()) {
      std::filesystem::create_directories(target);
      continue;
    }
    std::filesystem::create_directories(target.parent_path());
    WriteFile(target, ReadFile(entry.path()));
  }
}

std::string ShellEscape(std::string_view text) {
  std::string escaped;
  escaped.reserve(text.size() + 8);
  for (char c : text) {
    if (c == '\'') {
      escaped += "'\\''";
    } else {
      escaped.push_back(c);
    }
  }
  return escaped;
}

std::string EscapedRepoPath(const std::filesystem::path& repo_path) {
  std::string path = repo_path.lexically_normal().generic_string();
  std::replace(path.begin(), path.end(), '\\', '/');
  return ShellEscape(path);
}

int RunCommand(const std::string& command) {
  return std::system(command.c_str());
}

void RequireCommandSuccess(const std::string& command, std::string_view context) {
  if (RunCommand(command) != 0) {
    throw std::runtime_error(std::string(context) + ": command failed: " + command);
  }
}

int RunGitCommand(const std::filesystem::path& repo_path, const std::vector<std::string>& args) {
  platform::SubprocessOptions options;
  options.cwd = repo_path;
  options.capture_stdout = false;
  options.capture_stderr = false;

  std::vector<std::string> command;
  command.reserve(args.size() + 1);
  command.emplace_back("git");
  command.insert(command.end(), args.begin(), args.end());
  return platform::RunSubprocess(command, options).exit_code;
}

void RequireGitCommandSuccess(const std::filesystem::path& repo_path,
                              const std::vector<std::string>& args,
                              std::string_view context) {
  if (RunGitCommand(repo_path, args) != 0) {
    std::ostringstream command_text;
    command_text << "git";
    for (const auto& arg : args) {
      command_text << ' ' << arg;
    }
    throw std::runtime_error(std::string(context) + ": command failed: " + command_text.str());
  }
}

void InitializeGitRepo(const std::filesystem::path& repo_path) {
  std::filesystem::create_directories(repo_path);
  RequireGitCommandSuccess(repo_path, {"-c", "init.defaultBranch=main", "init", "."}, "git init");
  RequireGitCommandSuccess(repo_path, {"config", "user.name", "Microide Tests"},
                           "git config user.name");
  RequireGitCommandSuccess(repo_path, {"config", "user.email", "microide-tests@example.com"},
                           "git config user.email");
}

void CommitAll(const std::filesystem::path& repo_path,
               std::string_view message,
               std::string_view context) {
  RequireGitCommandSuccess(repo_path, {"add", "."}, std::string(context) + " add");
  RequireGitCommandSuccess(repo_path, {"commit", "-m", std::string(message)},
                           std::string(context) + " commit");
}

void WritePluginInit(const std::filesystem::path& root, std::string_view directory_name,
                     std::string_view content) {
  WriteFile(root / directory_name / "init.lua", std::string(content));
}

TemporaryDirectory::TemporaryDirectory() {
  static std::atomic<unsigned long long> counter{0};
  const auto base = std::filesystem::temp_directory_path();
  for (;;) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto id = counter.fetch_add(1, std::memory_order_relaxed);
    const auto candidate =
        base / ("microide-tests-" + std::to_string(stamp) + "-" + std::to_string(id));
    std::error_code error;
    if (std::filesystem::create_directories(candidate, error) && !error) {
      path_ = candidate;
      break;
    }
  }
}

TemporaryDirectory::~TemporaryDirectory() {
  std::error_code error;
  std::filesystem::remove_all(path_, error);
}

ScopedEnvVar::ScopedEnvVar(std::string name, std::string value) : name_(std::move(name)) {
  const char* previous = std::getenv(name_.c_str());
  if (previous != nullptr) {
    had_previous_ = true;
    previous_value_ = previous;
  }
  Set(value);
}

ScopedEnvVar::~ScopedEnvVar() {
  if (had_previous_) {
    Set(previous_value_);
    return;
  }
  unsetenv(name_.c_str());
}

void ScopedEnvVar::Set(const std::string& value) {
  setenv(name_.c_str(), value.c_str(), 1);
}

}  // namespace microide::tests
