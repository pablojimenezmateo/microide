#pragma once

#include <filesystem>
#include <string>
#include <vector>

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
};

}  // namespace microide::plugin
