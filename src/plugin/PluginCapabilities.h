#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "platform/ProcessLauncher.h"
#include "project/FileWriteGate.h"
#include "util/PathMatch.h"

namespace microide::plugin {

// Filesystem reach a plugin may have through ctx.files.*. Default is project-scoped:
// the plugin may touch the active project tree but nothing above it. kProjectAndData
// also grants the plugin's own writable data directory; kNone denies all file access.
enum class FsAccess : unsigned char {
  kNone = 0,
  kProjectScoped = 1,
  kProjectAndData = 2,
};

// Per-plugin capability set, declared in the plugin's manifest table
// (`capabilities = { ... }` in init.lua) and enforced at the fs/process chokepoints.
// Defaults intentionally encode the trust posture: filesystem is project-scoped, but
// process execution and network are default-deny — a plugin must opt in explicitly.
struct PluginCapabilities {
  FsAccess fs_read = FsAccess::kProjectScoped;
  FsAccess fs_write = FsAccess::kProjectScoped;

  // ctx.process.run / run_async and any spawnable contribution (formatter / language
  // server / task) require process_exec. An empty allowlist with process_exec=true means
  // "any binary"; a non-empty allowlist restricts argv[0] to a basename or absolute-path match.
  bool process_exec = false;
  std::vector<std::string> process_allowlist;

  // Declared now, enforced once a network host API exists. Surfaced so manifests are
  // forward-compatible and the kernel-hardening layer can deny sockets when false.
  bool network = false;
};

// Non-owning view of the calling plugin's filesystem/process scope, threaded from the
// Lua-API wrapper layer down into the interop chokepoints. Holds only references, so it
// is trivially destructible: it is safe to construct in a wrapper frame whose delegated
// call may raise a Lua error (a C longjmp). See src/plugin/LuaError.h.
struct PluginFsContext {
  const std::filesystem::path& project_root;
  const std::filesystem::path& data_dir;
  const PluginCapabilities& caps;
  // The project's locality (PluginHost::ProjectLocality). Not used directly:
  // locality is a property of the PATH an operation targets, so callers go through
  // LauncherFor / WriteGateFor (TD-2026-10-06-318).
  const platform::ProcessLauncher& project_launcher;
  project::FileWriteGate& project_write_gate;

  // A path inside the project tree belongs to the project — in a remote project it
  // lives on the host, and its local copy is the mirror. Anything else a plugin may
  // reach (its own data directory) is this machine's. `path` must already be
  // resolved and contained, which is what both callers hold.
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

}  // namespace microide::plugin
