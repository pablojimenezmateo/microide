#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "platform/ProcessLauncher.h"
#include "project/FileWriteGate.h"

namespace microide::plugin {

using ResolvedPluginSettings = std::vector<std::pair<std::string, std::string>>;

struct PluginHostSnapshot {
  struct ActiveBuffer {
    std::filesystem::path path;
    std::size_t line = 0;
    std::size_t column = 0;
    bool present = false;
  };

  std::filesystem::path project_root;
  const platform::ProcessLauncher* project_launcher = &platform::LocalProcessLauncher();
  project::FileWriteGate* project_write_gate = &project::LocalFileWriteGate();
  ActiveBuffer active_buffer;
  std::shared_ptr<const ResolvedPluginSettings> settings;
  std::uint64_t generation = 0;
};

using PluginMainThreadAction = std::function<void()>;

}
