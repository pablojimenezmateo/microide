#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "platform/ProcessLauncher.h"
#include "project/FileWriteGate.h"
#include "util/PathMatch.h"

namespace microide::plugin {

enum class FsAccess : unsigned char {
  kNone = 0,
  kProjectScoped = 1,
  kProjectAndData = 2,
};

struct PluginCapabilities {
  FsAccess fs_read = FsAccess::kProjectScoped;
  FsAccess fs_write = FsAccess::kProjectScoped;

  bool process_exec = false;
  std::vector<std::string> process_allowlist;

  bool network = false;
};

struct PluginFsContext {
  const std::filesystem::path& project_root;
  const std::filesystem::path& data_dir;
  const PluginCapabilities& caps;
  const platform::ProcessLauncher& project_launcher;
  project::FileWriteGate& project_write_gate;

  [[nodiscard]] bool InProjectTree(const std::filesystem::path& path) const {
    return !project_root.empty() && util::NormalizedPathEqualsOrWithin(path, project_root);
  }
  [[nodiscard]] const platform::ProcessLauncher& LauncherFor(
      const std::filesystem::path& cwd) const {
    return InProjectTree(cwd) ? project_launcher : platform::LocalProcessLauncher();
  }
  [[nodiscard]] project::FileWriteGate& WriteGateFor(const std::filesystem::path& path) const {
    return InProjectTree(path) ? project_write_gate : project::LocalFileWriteGate();
  }
};

}
