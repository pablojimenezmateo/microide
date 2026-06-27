#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "platform/SubprocessSandbox.h"

namespace microide::platform {

struct SubprocessEnvironmentOverride {
  std::string name;
  std::optional<std::string> value;
};

struct SubprocessOptions {
  std::filesystem::path cwd;
  std::string stdin_text;
  std::vector<SubprocessEnvironmentOverride> environment_overrides;
  bool capture_stdout = true;
  bool capture_stderr = true;
  bool silence_stderr = false;
  // Wall-clock cap on the whole run. 0 (default) waits indefinitely -- the
  // historical behavior every existing caller relies on. When > 0 and the child
  // outlives the deadline it is SIGKILLed; the result reports timed_out and a
  // non-zero exit_code. Currently honored on POSIX only.
  int timeout_ms = 0;
  // Optional kernel-level confinement applied in the forked child before exec. Disabled by
  // default so host-internal subprocesses (git, ripgrep, …) are unaffected; plugin spawns opt in.
  SubprocessSandbox sandbox{};
};

struct SubprocessResult {
  int exit_code = -1;
  std::string stdout_text;
  std::string stderr_text;
  // Set when the run exceeded SubprocessOptions::timeout_ms and was killed.
  bool timed_out = false;

  bool success() const { return exit_code == 0; }
};

SubprocessResult RunSubprocess(const std::vector<std::string>& argv,
                               const SubprocessOptions& options = {});

}  // namespace microide::platform
