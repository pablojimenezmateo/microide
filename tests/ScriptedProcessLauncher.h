#pragma once

#include "platform/ProcessLauncher.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace microide::tests {

class ScriptedProcessLauncher final : public platform::ProcessLauncher {
 public:
  struct Response {
    int exit_code = 0;
    std::string stdout_text;
    std::string stderr_text;
    bool timed_out = false;
    bool truncated = false;
  };

  mutable std::vector<std::vector<std::string>> runs;
  mutable std::vector<std::filesystem::path> run_cwds;

  mutable std::vector<std::vector<std::string>> resolved_argvs;

  Response standing_response;

  mutable std::vector<Response> queued_responses;

  ScriptedProcessLauncher() = default;
  explicit ScriptedProcessLauncher(Response response)
      : standing_response(std::move(response)) {}

  static ScriptedProcessLauncher MissingProgram() {
    return ScriptedProcessLauncher(Response{.exit_code = 127,
                                            .stderr_text = "execvp: No such file or directory"});
  }

  std::vector<std::string> ResolveArgv(std::vector<std::string> argv) const override {
    resolved_argvs.push_back(argv);
    return argv;
  }
  std::filesystem::path ResolveWorkingDirectory(std::filesystem::path cwd) const override {
    return cwd;
  }
  std::filesystem::path LocalPathFromHost(std::filesystem::path host_path) const override {
    return host_path;
  }
  platform::SubprocessResult Run(std::vector<std::string> argv,
                                 platform::SubprocessOptions options) const override {
    runs.push_back(std::move(argv));
    run_cwds.push_back(std::move(options.cwd));
    Response response = standing_response;
    if (!queued_responses.empty()) {
      response = queued_responses.front();
      queued_responses.erase(queued_responses.begin());
    }
    return platform::SubprocessResult{
        .exit_code = response.exit_code,
        .stdout_text = std::move(response.stdout_text),
        .stderr_text = std::move(response.stderr_text),
        .timed_out = response.timed_out,
        .truncated = response.truncated,
    };
  }
  bool is_local() const override { return local; }
  std::string_view description() const override { return "scripted"; }

  bool local = true;

  [[nodiscard]] bool AnyRunContains(std::string_view word) const {
    for (const std::vector<std::string>& argv : runs) {
      for (const std::string& entry : argv) {
        if (entry == word) {
          return true;
        }
      }
    }
    return false;
  }
};

}
