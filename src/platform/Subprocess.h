#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace microide::platform {

struct SubprocessOptions {
  std::filesystem::path cwd;
  std::string stdin_text;
  bool capture_stdout = true;
  bool capture_stderr = true;
  bool silence_stderr = false;
};

struct SubprocessResult {
  int exit_code = -1;
  std::string stdout_text;
  std::string stderr_text;

  bool success() const { return exit_code == 0; }
};

SubprocessResult RunSubprocess(const std::vector<std::string>& argv,
                               const SubprocessOptions& options = {});

}  // namespace microide::platform
