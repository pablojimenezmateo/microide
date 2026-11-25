#pragma once

#include <filesystem>
#include <string_view>
#include <vector>

namespace microide::platform {

struct SubprocessResourceLimits {
  long cpu_seconds = 0;
  long address_space_bytes = 0;
  long open_files = 0;
  long file_size_bytes = 0;
};

struct SubprocessSandbox {
  bool enabled = false;
  std::vector<std::filesystem::path> read_roots;
  std::vector<std::filesystem::path> read_files;
  std::vector<std::filesystem::path> write_roots;
  bool allow_network = true;
  SubprocessResourceLimits limits;
};

void ApplyChildSandbox(const SubprocessSandbox& sandbox);

void AllowUserGitConfiguration(SubprocessSandbox& sandbox);

void AllowUserToolchains(SubprocessSandbox& sandbox, std::string_view path_env,
                         const std::filesystem::path& home);

struct SandboxSupport {
  bool compiled_with_landlock = false;
  bool landlock_runtime_available = false;
  int landlock_abi = 0;
  bool compiled_with_seccomp = false;
  bool seccomp_runtime_available = false;

  bool fully_active() const {
    return landlock_runtime_available &&
           (compiled_with_seccomp ? seccomp_runtime_available : true);
  }
};

SandboxSupport ProbeSandboxSupport();

}
