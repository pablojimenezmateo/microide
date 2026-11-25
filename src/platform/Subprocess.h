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
  int timeout_ms = 0;
  SubprocessSandbox sandbox{};
};

inline constexpr std::size_t kMaxSubprocessCaptureBytes = 128ull * 1024 * 1024;

struct SubprocessResult {
  int exit_code = -1;
  std::string stdout_text;
  std::string stderr_text;
  bool timed_out = false;
  bool truncated = false;

  bool success() const { return exit_code == 0; }
};

SubprocessResult RunSubprocess(const std::vector<std::string>& argv,
                               const SubprocessOptions& options = {});

}
