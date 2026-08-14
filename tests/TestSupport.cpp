#include "TestSupport.h"

#include "platform/Subprocess.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <algorithm>
#include <fstream>
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
#if defined(_WIN32)
  if (path.size() >= 3 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':' &&
      path[2] == '/') {
    path = "/" + std::string(1, static_cast<char>(std::tolower(static_cast<unsigned char>(path[0])))) +
           path.substr(2);
  }
#endif
  return ShellEscape(path);
}

int RunCommand(const std::string& command) {
#if defined(_WIN32)
  platform::SubprocessOptions options;
  options.capture_stdout = false;
  options.capture_stderr = false;
  return platform::RunSubprocess({"C:\\msys64\\usr\\bin\\bash.exe", "-lc", command}, options).exit_code;
#else
  return std::system(command.c_str());
#endif
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
#if defined(_WIN32)
  _putenv_s(name_.c_str(), "");
#else
  unsetenv(name_.c_str());
#endif
}

void ScopedEnvVar::Set(const std::string& value) {
#if defined(_WIN32)
  _putenv_s(name_.c_str(), value.c_str());
#else
  setenv(name_.c_str(), value.c_str(), 1);
#endif
}

ScopedHostPlatformOverride::ScopedHostPlatformOverride(platform::HostPlatform platform)
    : previous_(microide::platform::CurrentHostPlatform()) {
  microide::platform::SetHostPlatformOverrideForTesting(platform);
}

ScopedHostPlatformOverride::~ScopedHostPlatformOverride() {
  microide::platform::SetHostPlatformOverrideForTesting(previous_);
}

void EnsureDummySdlVideoInitialized() {
  static ScopedEnvVar video_driver("SDL_VIDEODRIVER", "dummy");
  static const bool initialized = SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
  if (!initialized) {
    throw std::runtime_error(
        std::string("SDL initialization failed for tests: ") + SDL_GetError());
  }
}

void ResetSdlModStateForTests() {
  SDL_SetModState(SDL_KMOD_NONE);
}

ScopedSdlModState::ScopedSdlModState(SDL_Keymod modifiers) : previous_mods_(SDL_GetModState()) {
  SDL_SetModState(modifiers);
}

ScopedSdlModState::~ScopedSdlModState() {
  SDL_SetModState(previous_mods_);
}

}  // namespace microide::tests
