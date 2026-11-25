#include "plugin/PluginProcessInterop.h"

#if MICROIDE_HAS_LUA_PLUGINS

#include <array>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "platform/Subprocess.h"
#include "platform/ProcessLauncher.h"
#include "plugin/LuaError.h"
#include "plugin/LuaRuntime.h"
#include "plugin/PluginLuaInterop.h"
#include "plugin/PluginPathInterop.h"

namespace microide::plugin::process_interop {
namespace {

using path_interop::ContainPath;
using path_interop::ResolveRuntimePath;

constexpr int kPluginProcessTimeoutMs = 120'000;

struct ProcessRunArgs {
  std::vector<std::string> argv;
  std::filesystem::path cwd;
  std::string stdin_text;
  std::vector<platform::SubprocessEnvironmentOverride> environment_overrides;
};

const char* ParseProcessRunArgs(lua_State* state,
                                const std::filesystem::path& current_project_root,
                                ProcessRunArgs* out) {
  if (lua_type(state, 1) != LUA_TTABLE) {
    return "process argv must be a table";
  }
  constexpr lua_Integer kMaxProcessArgv = 4096;
  const lua_Integer argc =
      std::min<lua_Integer>(static_cast<lua_Integer>(lua_rawlen(state, 1)), kMaxProcessArgv);
  out->argv.reserve(static_cast<std::size_t>(argc));
  for (lua_Integer i = 1; i <= argc; ++i) {
    lua_rawgeti(state, 1, i);
    if (!lua_isstring(state, -1)) {
      lua_pop(state, 1);
      return "process argv entries must be strings";
    }
    std::optional<std::string> arg = lua_interop::ToHostString(state, -1);
    lua_pop(state, 1);
    if (!arg) {
      return "process argv entries must be NUL-free strings";
    }
    out->argv.push_back(std::move(*arg));
  }

  out->cwd = current_project_root;
  if (lua_gettop(state) >= 2 && !lua_isnil(state, 2)) {
    if (lua_type(state, 2) != LUA_TTABLE) {
      return "process options must be a table";
    }
    lua_interop::GetFieldProtected(state, 2, "cwd");
    if (lua_isstring(state, -1)) {
      std::optional<std::string> cwd_text = lua_interop::ToHostString(state, -1);
      if (!cwd_text) {
        lua_pop(state, 1);
        return "process cwd must be a NUL-free string";
      }
      out->cwd = ResolveRuntimePath(current_project_root, std::filesystem::path(*cwd_text));
    }
    lua_pop(state, 1);

    lua_interop::GetFieldProtected(state, 2, "stdin");
    if (lua_isstring(state, -1)) {
      size_t length = 0;
      const char* text = lua_tolstring(state, -1, &length);
      constexpr size_t kMaxProcessStdinBytes = 16 * 1024 * 1024;
      if (length > kMaxProcessStdinBytes) {
        lua_pop(state, 1);
        return "process stdin exceeds the 16 MiB limit";
      }
      out->stdin_text.assign(text, length);
    }
    lua_pop(state, 1);

    lua_interop::GetFieldProtected(state, 2, "env");
    if (!lua_isnil(state, -1)) {
      if (lua_type(state, -1) != LUA_TTABLE) {
        lua_pop(state, 1);
        return "process env must be a table";
      }
      constexpr std::size_t kMaxProcessEnvEntries = 4096;
      constexpr std::size_t kMaxProcessEnvBytes = 1u << 20;
      std::size_t env_bytes = 0;
      lua_pushnil(state);
      while (lua_next(state, -2) != 0) {
        if (out->environment_overrides.size() >= kMaxProcessEnvEntries) {
          lua_pop(state, 2);
          return "process env exceeds the maximum number of entries";
        }
        if (lua_type(state, -2) != LUA_TSTRING) {
          lua_pop(state, 2);
          return "process env keys must be strings";
        }

        platform::SubprocessEnvironmentOverride override_entry;
        {
          std::size_t name_len = 0;
          const char* name_ptr = lua_tolstring(state, -2, &name_len);
          override_entry.name.assign(name_ptr, name_len);
        }
        if (override_entry.name.empty() ||
            override_entry.name.find('\0') != std::string::npos ||
            override_entry.name.find('=') != std::string::npos) {
          lua_pop(state, 2);
          return "process env keys must be non-empty and contain no '=' or NUL byte";
        }
        if (lua_isstring(state, -1)) {
          size_t length = 0;
          const char* text = lua_tolstring(state, -1, &length);
          override_entry.value = std::string(text, length);
        } else if (lua_isboolean(state, -1) && lua_toboolean(state, -1) == 0) {
          override_entry.value = std::nullopt;
        } else {
          lua_pop(state, 2);
          return "process env values must be strings or false";
        }

        env_bytes += override_entry.name.size() +
                     (override_entry.value ? override_entry.value->size() : 0);
        if (env_bytes > kMaxProcessEnvBytes) {
          lua_pop(state, 2);
          return "process env exceeds the maximum total size";
        }

        out->environment_overrides.push_back(std::move(override_entry));
        lua_pop(state, 1);
      }
    }
    lua_pop(state, 1);
  }

  return nullptr;
}

bool ProgramAllowed(const PluginCapabilities& caps, const std::vector<std::string>& argv) {
  if (caps.process_allowlist.empty()) {
    return true;
  }
  if (argv.empty()) {
    return false;
  }
  const std::string base = std::filesystem::path(argv[0]).filename().string();
  for (const std::string& allowed : caps.process_allowlist) {
    if (allowed == argv[0] || allowed == base) {
      return true;
    }
  }
  return false;
}

bool CwdContained(const PluginFsContext& fs, const std::filesystem::path& cwd) {
  if (cwd.empty()) {
    return true;
  }
  const std::array<std::filesystem::path, 2> roots{fs.project_root, fs.data_dir};
  return ContainPath(std::span(roots), cwd).has_value();
}

platform::SubprocessSandbox MakeSandbox(const PluginFsContext& fs) {
  platform::SubprocessSandbox sandbox;
  sandbox.enabled = true;
  sandbox.allow_network = fs.caps.network;
  const auto add_roots_for_level = [&fs](FsAccess level,
                                         std::vector<std::filesystem::path>& roots) {
    if (level == FsAccess::kNone) {
      return;
    }
    if (!fs.project_root.empty()) {
      roots.push_back(fs.project_root);
    }
    if (level == FsAccess::kProjectAndData && !fs.data_dir.empty()) {
      roots.push_back(fs.data_dir);
    }
  };
  add_roots_for_level(fs.caps.fs_read, sandbox.read_roots);
  add_roots_for_level(fs.caps.fs_write, sandbox.write_roots);
  platform::AllowUserGitConfiguration(sandbox);
  return sandbox;
}

const char* CheckProcessCapability(const PluginFsContext& fs, const ProcessRunArgs& parsed) {
  if (!fs.caps.process_exec) {
    return "process execution not permitted; declare capabilities.process.exec";
  }
  if (parsed.argv.empty()) {
    return "process argv must not be empty";
  }
  if (!ProgramAllowed(fs.caps, parsed.argv)) {
    return "process program not in plugin allowlist (capabilities.process.allow)";
  }
  if (!CwdContained(fs, parsed.cwd)) {
    return "process cwd escapes plugin filesystem scope";
  }
  return nullptr;
}

}

static platform::SubprocessResult RunPluginSubprocess(const PluginFsContext& fs,
                                                      ProcessRunArgs& parsed) {
  const platform::ProcessLauncher& launcher = fs.LauncherFor(parsed.cwd);
  return launcher.Run(
      platform::ExpandWorkspaceFolder(parsed.argv, fs.project_root, launcher),
      platform::SubprocessOptions{
                       .cwd = parsed.cwd,
                       .stdin_text = parsed.stdin_text,
                       .environment_overrides = std::move(parsed.environment_overrides),
                       .capture_stdout = true,
                       .capture_stderr = true,
                       .timeout_ms = kPluginProcessTimeoutMs,
                       .sandbox = MakeSandbox(fs),
                   });
}

static void PushProcessResultTable(lua_State* state, const platform::SubprocessResult& result) {
  lua_createtable(state, 0, 4);
  lua_pushinteger(state, result.exit_code);
  lua_setfield(state, -2, "exit_code");
  lua_pushboolean(state, result.exit_code == 0 ? 1 : 0);
  lua_setfield(state, -2, "ok");
  lua_pushlstring(state, result.stdout_text.c_str(), result.stdout_text.size());
  lua_setfield(state, -2, "stdout");
  lua_pushlstring(state, result.stderr_text.c_str(), result.stderr_text.size());
  lua_setfield(state, -2, "stderr");
}

int LuaProcessRun(lua_State* state, const PluginFsContext& fs) {
  const char* error = nullptr;
  {
    ProcessRunArgs parsed;
    error = ParseProcessRunArgs(state, fs.project_root, &parsed);
    if (error == nullptr) {
      error = CheckProcessCapability(fs, parsed);
    }
    if (error == nullptr) {
      const platform::SubprocessResult result = RunPluginSubprocess(fs, parsed);
      PushProcessResultTable(state, result);
      return 1;
    }
  }
  lua_error_util::PushMessage(state, error);
  return lua_error_util::kPendingError;
}

int LuaProcessRunAsync(lua_State* state, const PluginFsContext& fs) {
  if (lua_type(state, 3) != LUA_TFUNCTION) {
    lua_error_util::PushMessage(state, "process.run_async requires a callback function");
    return lua_error_util::kPendingError;
  }

  const char* error = nullptr;
  bool callback_failed = false;
  std::string callback_error;
  {
    ProcessRunArgs parsed;
    error = ParseProcessRunArgs(state, fs.project_root, &parsed);
    if (error == nullptr) {
      error = CheckProcessCapability(fs, parsed);
    }
    if (error == nullptr) {
      const platform::SubprocessResult result = RunPluginSubprocess(fs, parsed);
      lua_pushvalue(state, 3);
      PushProcessResultTable(state, result);
      LuaRuntime* runtime = *static_cast<LuaRuntime**>(lua_getextraspace(state));
      callback_failed = runtime == nullptr || !runtime->PCallNested(1, 0, &callback_error);
    }
  }
  if (error != nullptr) {
    lua_error_util::PushMessage(state, error);
    return lua_error_util::kPendingError;
  }
  if (callback_failed) {
    lua_error_util::PushMessage(state, callback_error, "process.run_async callback failed");
    return lua_error_util::kPendingError;
  }
  return 0;
}

}

#endif
