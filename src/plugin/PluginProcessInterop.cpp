#include "plugin/PluginProcessInterop.h"

#if MICROIDE_HAS_LUA_PLUGINS

#include <array>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "platform/Subprocess.h"
#include "plugin/LuaError.h"
#include "plugin/LuaRuntime.h"
#include "plugin/PluginLuaInterop.h"
#include "plugin/PluginPathInterop.h"

namespace microide::plugin::process_interop {
namespace {

using path_interop::ContainPath;
using path_interop::ResolveRuntimePath;

// Backstop wall-clock cap on a plugin-spawned subprocess. A hung child (a tool
// waiting on input, a stuck network call) would otherwise block the plugin worker
// thread forever, stalling every subsequent plugin task behind it. Generous
// enough for normal formatters/linters; run_async is the path for genuinely
// long-lived work, and it too gets this backstop rather than an unbounded wait.
constexpr int kPluginProcessTimeoutMs = 120'000;

// Native form of the `process.run` arguments. Holds every heap-backed object the
// call needs so the parse step can fail without leaving such an object alive on
// the stack when the caller raises a Lua error (a C longjmp — see LuaError.h).
struct ProcessRunArgs {
  std::vector<std::string> argv;
  std::filesystem::path cwd;
  std::string stdin_text;
  std::vector<platform::SubprocessEnvironmentOverride> environment_overrides;
};

// Parses the argv table (index 1) and optional options table (index 2) into
// `out`. Returns nullptr on success, or a static error-message literal on a type
// mismatch. Never calls luaL_error / luaL_checktype: those longjmp, which would
// skip the destructors of `out`'s std::vector / std::string members. The caller
// raises the returned literal only after `out` has gone out of scope.
const char* ParseProcessRunArgs(lua_State* state,
                                const std::filesystem::path& current_project_root,
                                ProcessRunArgs* out) {
  if (lua_type(state, 1) != LUA_TTABLE) {
    return "process argv must be a table";
  }
  // Clamp against a sparse-border table making lua_rawlen overstate the length
  // (no real command needs thousands of argv entries): bound reserve and loop.
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
    out->argv.emplace_back(lua_tostring(state, -1));
    lua_pop(state, 1);
  }

  out->cwd = current_project_root;
  if (lua_gettop(state) >= 2 && !lua_isnil(state, 2)) {
    if (lua_type(state, 2) != LUA_TTABLE) {
      return "process options must be a table";
    }
    lua_interop::GetFieldProtected(state, 2, "cwd");
    if (lua_isstring(state, -1)) {
      out->cwd =
          ResolveRuntimePath(current_project_root, std::filesystem::path(lua_tostring(state, -1)));
    }
    lua_pop(state, 1);

    lua_interop::GetFieldProtected(state, 2, "stdin");
    if (lua_isstring(state, -1)) {
      size_t length = 0;
      const char* text = lua_tolstring(state, -1, &length);
      out->stdin_text.assign(text, length);
    }
    lua_pop(state, 1);

    lua_interop::GetFieldProtected(state, 2, "env");
    if (!lua_isnil(state, -1)) {
      if (lua_type(state, -1) != LUA_TTABLE) {
        lua_pop(state, 1);
        return "process env must be a table";
      }
      // Bound the env drain the same way argv is bounded above: lua_next is
      // unbounded, so a malformed/hostile table could otherwise grow
      // `environment_overrides` (and its backing key/value strings) without limit
      // on the plugin worker thread. Cap both the entry count and the total
      // key+value byte volume, rejecting once either is exceeded.
      constexpr std::size_t kMaxProcessEnvEntries = 4096;
      constexpr std::size_t kMaxProcessEnvBytes = 1u << 20;  // 1 MiB of key+value data
      std::size_t env_bytes = 0;
      lua_pushnil(state);
      while (lua_next(state, -2) != 0) {
        if (out->environment_overrides.size() >= kMaxProcessEnvEntries) {
          lua_pop(state, 2);
          return "process env exceeds the maximum number of entries";
        }
        // Strict string check on the KEY: lua_isstring() is also true for numbers,
        // and calling lua_tostring() on a numeric key converts it in place, which
        // corrupts the running lua_next() iteration (Lua raises "invalid key to
        // 'next'"). That raise longjmps over the live ProcessRunArgs local. Reject
        // non-string keys before touching them, matching the theme parser.
        if (lua_type(state, -2) != LUA_TSTRING) {
          lua_pop(state, 2);
          return "process env keys must be strings";
        }

        platform::SubprocessEnvironmentOverride override_entry;
        override_entry.name = lua_tostring(state, -2);
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

// True when argv[0] satisfies the plugin's allowlist. An empty allowlist (with exec granted)
// permits any binary; otherwise argv[0] must match an entry exactly (e.g. an absolute path) or
// by basename, so a plugin allowing "eslint" accepts both "eslint" and "/usr/bin/eslint".
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

// A spawned process may only be cwd'd inside the plugin's project root or data directory. An
// empty cwd means "inherit the host's working directory" and is left untouched.
bool CwdContained(const PluginFsContext& fs, const std::filesystem::path& cwd) {
  if (cwd.empty()) {
    return true;
  }
  const std::array<std::filesystem::path, 2> roots{fs.project_root, fs.data_dir};
  return ContainPath(std::span(roots), cwd).has_value();
}

// Builds the kernel-confinement descriptor for a plugin-spawned child: writes are limited to the
// project root and the plugin data dir, and IPv4/IPv6 sockets are blocked unless the plugin
// declared the network capability. The wider system stays readable/executable so the tool can run.
platform::SubprocessSandbox MakeSandbox(const PluginFsContext& fs) {
  platform::SubprocessSandbox sandbox;
  sandbox.enabled = true;
  sandbox.allow_network = fs.caps.network;
  if (!fs.project_root.empty()) {
    sandbox.read_roots.push_back(fs.project_root);
    sandbox.write_roots.push_back(fs.project_root);
  }
  if (!fs.data_dir.empty()) {
    sandbox.read_roots.push_back(fs.data_dir);
    sandbox.write_roots.push_back(fs.data_dir);
  }
  return sandbox;
}

// Returns nullptr when the parsed call is permitted, or a static error-message literal to be
// raised. Holds no heap state of its own so the caller can raise after `parsed` destructs.
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

}  // namespace

int LuaProcessRun(lua_State* state, const PluginFsContext& fs) {
  const char* error = nullptr;
  {
    ProcessRunArgs parsed;
    error = ParseProcessRunArgs(state, fs.project_root, &parsed);
    if (error == nullptr) {
      error = CheckProcessCapability(fs, parsed);
    }
    if (error == nullptr) {
      const platform::SubprocessResult result = platform::RunSubprocess(
          parsed.argv, platform::SubprocessOptions{
                           .cwd = parsed.cwd,
                           .stdin_text = parsed.stdin_text,
                           .environment_overrides = std::move(parsed.environment_overrides),
                           .capture_stdout = true,
                           .capture_stderr = true,
                           .timeout_ms = kPluginProcessTimeoutMs,
                           .sandbox = MakeSandbox(fs),
                       });
      lua_createtable(state, 0, 4);
      lua_pushinteger(state, result.exit_code);
      lua_setfield(state, -2, "exit_code");
      lua_pushboolean(state, result.exit_code == 0 ? 1 : 0);
      lua_setfield(state, -2, "ok");
      lua_pushlstring(state, result.stdout_text.c_str(), result.stdout_text.size());
      lua_setfield(state, -2, "stdout");
      lua_pushlstring(state, result.stderr_text.c_str(), result.stderr_text.size());
      lua_setfield(state, -2, "stderr");
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
      // Runs on the plugin worker, so blocking on the subprocess never stalls the
      // UI. The subprocess can legitimately outlast the enclosing call's watchdog
      // budget (that is the whole point of run_async), so the callback is invoked
      // through PCallNested, which gives it a fresh deadline instead of inheriting
      // the already-spent outer one — otherwise the watchdog would abort a healthy
      // callback on its first instruction. PCallNested is also protected, so a
      // callback error cannot longjmp over the C++ locals (`parsed`, `result`)
      // still alive here.
      const platform::SubprocessResult result = platform::RunSubprocess(
          parsed.argv, platform::SubprocessOptions{
                           .cwd = parsed.cwd,
                           .stdin_text = parsed.stdin_text,
                           .environment_overrides = std::move(parsed.environment_overrides),
                           .capture_stdout = true,
                           .capture_stderr = true,
                           .timeout_ms = kPluginProcessTimeoutMs,
                           .sandbox = MakeSandbox(fs),
                       });
      lua_pushvalue(state, 3);
      lua_createtable(state, 0, 4);
      lua_pushinteger(state, result.exit_code);
      lua_setfield(state, -2, "exit_code");
      lua_pushboolean(state, result.exit_code == 0 ? 1 : 0);
      lua_setfield(state, -2, "ok");
      lua_pushlstring(state, result.stdout_text.c_str(), result.stdout_text.size());
      lua_setfield(state, -2, "stdout");
      lua_pushlstring(state, result.stderr_text.c_str(), result.stderr_text.size());
      lua_setfield(state, -2, "stderr");
      LuaRuntime* runtime = *static_cast<LuaRuntime**>(lua_getextraspace(state));
      callback_failed = runtime == nullptr || !runtime->PCallNested(1, 0, &callback_error);
    }
  }
  if (error != nullptr) {
    lua_error_util::PushMessage(state, error);
    return lua_error_util::kPendingError;
  }
  if (callback_failed) {
    // PCallNested extracted the callback's error message into `callback_error`;
    // push it back for the wrapper, which records and raises it once this frame's
    // locals have destructed. (callback_error itself is gone by then — the raise
    // happens in the .inc wrapper after this function returns.)
    lua_error_util::PushMessage(state, callback_error, "process.run_async callback failed");
    return lua_error_util::kPendingError;
  }
  return 0;
}

}  // namespace microide::plugin::process_interop

#endif
