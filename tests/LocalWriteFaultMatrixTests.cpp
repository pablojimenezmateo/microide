// The local save fault matrix (safety-plan W5): a SIGKILL at every durable-write
// step of util::WriteTextFileAtomically -- the path every editor save, session and
// user-config write takes -- leaves the target holding its old bytes or its new
// ones, never a mix, a truncation or nothing.
//
// The write runs in a helper (this binary re-executed with --local-write-fault-
// client, as MirrorFaultMatrixTests does), which first counts the util::CrashPoint
// steps an undisturbed write passes and then, one run per step, SIGKILLs itself at
// that step. A killed write may leave its hidden staging temporary behind: the
// project scan sweeps those once their writer is gone (9ca11c328), so only the
// target's bytes are judged here.

#include "TestSupport.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "util/CrashPoint.h"
#include "util/Parse.h"
#include "util/TextFileIO.h"

namespace microide::tests {

namespace {

constexpr std::string_view kHelperFlag = "--local-write-fault-client";

std::atomic<std::size_t> g_steps{0};
std::atomic<std::size_t> g_kill_at{std::numeric_limits<std::size_t>::max()};

void KillAtStep(const char* /*site*/, std::string_view /*what*/) {
  if (g_steps.fetch_add(1) == g_kill_at.load()) {
    ::raise(SIGKILL);
  }
}

std::string ReadAll(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

struct HelperRun {
  bool killed = false;
  int exit_code = -1;
  std::size_t steps = 0;
};

// Runs the helper: write `content_file`'s bytes to `target` (mode create|replace),
// SIGKILLed at step `kill_at` (max: never). The helper prints its step count.
HelperRun RunHelper(const std::filesystem::path& target, const std::filesystem::path& content_file,
                    std::string_view mode, std::size_t kill_at) {
  int out[2];
  if (::pipe2(out, O_CLOEXEC) != 0) {
    return {};
  }
  const pid_t pid = ::fork();
  if (pid == 0) {
    ::dup2(out[1], STDOUT_FILENO);
    const std::string self = std::filesystem::read_symlink("/proc/self/exe").string();
    const std::string kill = std::to_string(kill_at);
    const std::string target_text = target.string();
    const std::string content_text = content_file.string();
    const std::string mode_text(mode);
    ::execl(self.c_str(), self.c_str(), std::string(kHelperFlag).c_str(), target_text.c_str(),
            content_text.c_str(), mode_text.c_str(), kill.c_str(), static_cast<char*>(nullptr));
    ::_exit(127);
  }
  ::close(out[1]);
  std::string printed;
  char buffer[64];
  for (ssize_t n; (n = ::read(out[0], buffer, sizeof(buffer))) > 0;) {
    printed.append(buffer, static_cast<std::size_t>(n));
  }
  ::close(out[0]);
  int status = 0;
  ::waitpid(pid, &status, 0);
  HelperRun run;
  run.killed = WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL;
  run.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  if (const auto steps = util::ParseSize(printed.substr(0, printed.find('\n'))); steps.has_value()) {
    run.steps = *steps;
  }
  return run;
}

struct Case {
  std::string name;
  std::optional<std::string> old_content;  // nullopt: the target does not exist yet
  std::string new_content;
  bool through_symlink = false;
};

void RunCase(const Case& c) {
  TemporaryDirectory temp;
  const std::filesystem::path dir = temp.path() / "project";
  std::filesystem::create_directories(dir);
  const std::filesystem::path content = temp.path() / "new-content";
  WriteFile(content, c.new_content);
  const auto reset = [&]() -> std::filesystem::path {
    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
    std::filesystem::create_directories(dir);
    const std::filesystem::path real = dir / "file.txt";
    if (c.old_content.has_value()) {
      WriteFile(real, *c.old_content);
    }
    if (c.through_symlink) {
      std::filesystem::create_symlink("file.txt", dir / "link.txt");
      return dir / "link.txt";
    }
    return real;
  };
  const std::string_view mode = c.old_content.has_value() ? "replace" : "create";
  const HelperRun clean = RunHelper(reset(), content, mode, std::numeric_limits<std::size_t>::max());
  Expect(clean.exit_code == 0 && clean.steps > 0,
         c.name + ": an undisturbed write succeeds and passes crash points (" + std::to_string(clean.steps) + ")");
  Expect(ReadAll(dir / "file.txt") == c.new_content, c.name + ": an undisturbed write leaves the new bytes");
  for (std::size_t step = 0; step < clean.steps; ++step) {
    const std::filesystem::path target = reset();
    const HelperRun run = RunHelper(target, content, mode, step);
    Expect(run.killed, c.name + ": the helper is killed at step " + std::to_string(step));
    std::error_code error;
    const bool exists = std::filesystem::exists(dir / "file.txt", error);
    const std::string now = exists ? ReadAll(dir / "file.txt") : std::string();
    const bool old_bytes = c.old_content.has_value() ? exists && now == *c.old_content : !exists;
    const bool new_bytes = exists && now == c.new_content;
    Expect(old_bytes || new_bytes, c.name + ": a kill at step " + std::to_string(step) + " of " +
                                       std::to_string(clean.steps) + " left " +
                                       (exists ? std::to_string(now.size()) + " bytes" : std::string("no file")) +
                                       ", neither the old content nor the new");
    if (c.through_symlink) {
      Expect(std::filesystem::is_symlink(target, error),
             c.name + ": the link survives a kill at step " + std::to_string(step));
    }
  }
}

void TestEveryCrashPointOfASaveLeavesTheOldOrTheNewFile() {
  std::string large;
  for (int i = 0; i < 40000; ++i) {
    large += "line " + std::to_string(i) + " of a large file\n";
  }
  for (const Case& c : std::vector<Case>{
           {"replace", std::string("old content\n"), "new content, longer than the old\n"},
           {"replace with a shorter file", std::string("a much longer old content\n"), "short\n"},
           {"replace a large file", std::string("old\n"), large},
           {"create", std::nullopt, "a new file's first save\n"},
           {"replace through a symlink", std::string("old through the link\n"), "new through the link\n", true},
       }) {
    RunCase(c);
  }
}

}  // namespace

bool IsLocalWriteFaultClient(int argc, char** argv) { return argc > 1 && std::string_view(argv[1]) == kHelperFlag; }

int RunLocalWriteFaultClient(int argc, char** argv) {
  if (argc != 6) {
    return 2;
  }
  const std::filesystem::path target = argv[2];
  const std::string content = ReadAll(argv[3]);
  const std::string_view mode = argv[4];
  const auto kill_at = util::ParseSize(argv[5]);
  if (!kill_at.has_value()) {
    return 2;
  }
  g_kill_at.store(*kill_at);
  util::SetCrashPointHook(&KillAtStep);
  std::error_code error;
  const bool ok = util::WriteTextFileAtomically(
      target, content, &error, mode == "create" ? util::WriteTarget::CreateNew : util::WriteTarget::Replace);
  util::SetCrashPointHook(nullptr);
  std::printf("%zu\n", g_steps.load());
  std::fflush(stdout);
  return ok ? 0 : 1;
}

void RegisterLocalWriteFaultMatrixTests(std::vector<TestCase>& tests) {
  AddTest(tests, "LocalWriteFaultMatrix/EveryCrashPointOfASaveLeavesTheOldOrTheNewFile",
          TestEveryCrashPointOfASaveLeavesTheOldOrTheNewFile);
}

}  // namespace microide::tests
