#include "TestSupport.h"

#include "plugin/PluginHost.h"
#include "plugin/PluginInstallRoot.h"
#include "plugin/PluginThread.h"
#include "plugin/LuaRuntime.h"
#include "workspace/WorkspacePluginAssetMonitor.h"

#include <chrono>
#include <algorithm>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace microide::tests {
namespace {

using microide::plugin::PluginHost;

void WritePluginInit(const std::filesystem::path& root,
                     std::string_view directory_name,
                     std::string_view content) {
  WriteFile(root / directory_name / "init.lua", std::string(content));
}

std::filesystem::path RepoPluginsRoot() {
  return TestRoot().parent_path() / "plugins";
}

void CopyRepoPlugin(const std::filesystem::path& root, std::string_view directory_name) {
  CopyTree(RepoPluginsRoot() / directory_name, root / directory_name);
}

PluginHost::Callbacks MakePluginHostCallbacks() {
  return PluginHost::Callbacks{
      .is_command_name_available = [](std::string_view) { return true; },
      .open_file = {},
      .active_buffer = {},
      .show_sidebar = {},
      .publish_diagnostics = {},
      .clear_file_diagnostics = {},
      .clear_owner_diagnostics = {},
      .error_sink = {},
      .log_sink = {},
      .get_setting = {},
      .request_status_redraw = {},
      .show_notification = {},
  };
}

class ScopedPluginConfigHomeEnv {
 public:
  explicit ScopedPluginConfigHomeEnv(const std::filesystem::path& config_home)
      : xdg_config_home_("XDG_CONFIG_HOME", config_home.string()),
        appdata_("APPDATA", config_home.string()) {}

 private:
  ScopedEnvVar xdg_config_home_;
  ScopedEnvVar appdata_;
};

void TestPluginHostLoadsPluginsAndDispatchesLifecycle() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path global_plugins = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  WriteFile(project_root / "README.md", "plugin host fixture\n");

  WritePluginInit(
      global_plugins, "global-sample",
      R"(local ide = require("microide")
return ide.plugin({
  id = "global.sample",
  setup = function(ctx)
    ctx.log("setup:global")
    ctx.commands.add("global.echo", function(ctx, args)
      ctx.log("command:global:" .. table.concat(args, ","))
    end)
  end,
  on_project_open = function(ctx, project)
    ctx.log("project-open:global:" .. project.name)
  end,
  on_project_close = function(ctx, project)
    ctx.log("project-close:global:" .. project.name)
  end,
  on_buffer_open = function(ctx, buffer)
    ctx.log("buffer-open:global:" .. buffer.relative_path)
  end,
  on_buffer_save = function(ctx, buffer)
    ctx.log("buffer-save:global:" .. buffer.relative_path)
  end,
  shutdown = function(ctx)
    ctx.log("shutdown:global")
  end
})
)");

  WritePluginInit(
      global_plugins, "secondary-sample",
      R"(local ide = require("microide")
return ide.plugin({
  id = "secondary.sample",
  setup = function(ctx)
    ctx.log("setup:secondary")
    ctx.commands.add("secondary.open-readme", function(ctx, args)
      ctx.workspace.open_file("README.md")
    end)
  end,
  on_project_open = function(ctx, project)
    ctx.log("project-open:secondary:" .. project.name)
  end,
  on_project_close = function(ctx, project)
    ctx.log("project-close:secondary:" .. project.name)
  end,
  on_buffer_open = function(ctx, buffer)
    ctx.log("buffer-open:secondary:" .. buffer.relative_path)
  end,
  on_buffer_save = function(ctx, buffer)
    ctx.log("buffer-save:secondary:" .. buffer.relative_path)
  end,
  shutdown = function(ctx)
    ctx.log("shutdown:secondary")
  end
})
)");

  ScopedPluginConfigHomeEnv config_env(config_home);

  std::vector<std::filesystem::path> opened_paths;
  PluginHost host;
  auto callbacks = MakePluginHostCallbacks();
  callbacks.is_command_name_available = [](std::string_view name) { return name != "quit"; };
  callbacks.open_file = [&](const PluginHost::OpenFileRequest& request) {
    opened_paths.push_back(request.path.lexically_normal());
    return true;
  };
  host.SetCallbacks(std::move(callbacks));

  Expect(host.enabled(), "plugin host should be enabled when Lua support is compiled in");
  Expect(host.Reload(project_root), "plugin reload should succeed for valid plugins");
  Expect(host.LoadedPluginCount() == 2, "plugin host should load both user-scope plugins");
  Expect(host.CommandNames().size() == 2, "plugin host should expose both registered commands");
  Expect(std::find(host.CommandNames().begin(), host.CommandNames().end(), "global.echo") !=
             host.CommandNames().end(),
         "plugin host should expose global plugin commands");
  Expect(std::find(host.CommandNames().begin(), host.CommandNames().end(), "secondary.open-readme") !=
             host.CommandNames().end(),
         "plugin host should expose secondary user-scope plugin commands");

  const std::vector<std::string>& load_messages = host.Messages();
  Expect(load_messages.size() >= 4, "plugin load should record setup and project-open messages");
  Expect(load_messages[0] == "global.sample: setup:global",
         "first user-scope plugin should set up before the second");
  Expect(load_messages[1] == "secondary.sample: setup:secondary",
         "second user-scope plugin setup should follow the first");
  Expect(load_messages[2] == "global.sample: project-open:global:project",
         "global project-open hook should run after setup");
  Expect(load_messages[3] == "secondary.sample: project-open:secondary:project",
         "second user-scope project-open hook should run after the first plugin");

  host.ClearMessages();
  host.OnBufferOpen(project_root / "src" / "main.cpp");
  host.OnBufferSave(project_root / "src" / "main.cpp");
  Expect(host.Messages().size() == 4,
         "buffer hooks should run for both loaded plugins");
  Expect(host.Messages()[0] == "global.sample: buffer-open:global:src/main.cpp",
         "global buffer-open hook should receive project-relative paths");
  Expect(host.Messages()[3] == "secondary.sample: buffer-save:secondary:src/main.cpp",
         "second user-scope buffer-save hook should receive project-relative paths");

  host.ClearMessages();
  std::string command_error;
  Expect(host.ExecuteCommand("global.echo", {"alpha", "beta"}, &command_error),
         "plugin command dispatch should succeed");
  Expect(command_error.empty(), "successful plugin commands should clear error output");
  Expect(!host.Messages().empty() &&
             host.Messages().back() == "global.sample: command:global:alpha,beta",
         "plugin commands should receive argv-style arguments");

  Expect(host.ExecuteCommand("secondary.open-readme", {}, &command_error),
         "workspace.open_file should be callable from plugin commands");
  Expect(!opened_paths.empty() && opened_paths.back() == (project_root / "README.md").lexically_normal(),
         "workspace.open_file should resolve relative paths against the active project");

  host.ClearMessages();
  host.Shutdown();
  Expect(host.Messages().size() == 4,
         "shutdown should emit project-close and shutdown hooks for each plugin");
  Expect(host.Messages()[0] == "global.sample: project-close:global:project",
         "shutdown should close the active project before tearing plugins down");
  Expect(host.Messages()[3] == "secondary.sample: shutdown:secondary",
         "shutdown should run each plugin's shutdown hook");
}

void TestPluginHostIgnoresProjectLocalPlugins() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path project_plugins = project_root / ".microide" / "plugins";

  WriteFile(project_root / "README.md", "project plugin ignore fixture\n");
  WritePluginInit(
      project_plugins, "evil",
      R"(local ide = require("microide")
return ide.plugin({
  id = "repo.evil",
  setup = function(ctx)
    ctx.commands.add("evil.run", function() end)
    ctx.sidebar.add({
      id = "evil.sidebar",
      label = "Evil",
      snapshot = function() return {} end
    })
  end
})
)");

  ScopedPluginConfigHomeEnv config_env(config_home);

  PluginHost host;
  host.SetCallbacks(MakePluginHostCallbacks());

  Expect(host.Reload(project_root), "plugin reload should succeed without project-local plugins");
  Expect(host.LoadedPluginCount() == 0,
         "project-local .microide/plugins directories should not be loaded");
  Expect(host.CommandNames().empty(),
         "project-local plugins should not register commands");
  Expect(host.FindSidebarProvider("evil.sidebar") == nullptr,
         "project-local plugins should not register sidebars");
}

void TestPluginHostLoadsUserConfigPlugins() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path user_plugins = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";

  WriteFile(project_root / "README.md", "user plugin load fixture\n");
  WritePluginInit(
      user_plugins, "good",
      R"(local ide = require("microide")
return ide.plugin({
  id = "user.good",
  setup = function(ctx)
    ctx.commands.add("good.echo", function() end)
    ctx.sidebar.add({
      id = "good.sidebar",
      label = "Good",
      snapshot = function() return {} end
    })
  end
})
)");

  ScopedPluginConfigHomeEnv config_env(config_home);

  PluginHost host;
  host.SetCallbacks(MakePluginHostCallbacks());

  Expect(host.Reload(project_root), "plugin reload should load user config plugins");
  Expect(host.LoadedPluginCount() == 1, "user config plugin should load");
  Expect(host.CommandNames().size() == 1 && host.CommandNames().front() == "good.echo",
         "user config plugin should register commands");
  Expect(host.FindSidebarProvider("good.sidebar") != nullptr,
         "user config plugin should register sidebars");
}

void TestPluginHostPrefersUserPluginOverProjectLocalPlugin() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path user_plugins = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path project_plugins = project_root / ".microide" / "plugins";

  WriteFile(project_root / "README.md", "user vs project plugin fixture\n");
  WritePluginInit(
      user_plugins, "good",
      R"(local ide = require("microide")
return ide.plugin({
  id = "user.good",
  setup = function(ctx)
    ctx.commands.add("good.echo", function() end)
  end
})
)");
  WritePluginInit(
      project_plugins, "evil",
      R"(local ide = require("microide")
return ide.plugin({
  id = "repo.evil",
  setup = function(ctx)
    ctx.commands.add("evil.run", function() end)
  end
})
)");

  ScopedPluginConfigHomeEnv config_env(config_home);

  PluginHost host;
  host.SetCallbacks(MakePluginHostCallbacks());

  Expect(host.Reload(project_root), "plugin reload should succeed with both plugin directories present");
  Expect(host.LoadedPluginCount() == 1, "only the user config plugin should load");
  Expect(host.CommandNames().size() == 1 && host.CommandNames().front() == "good.echo",
         "only user plugin registrations should appear");
  Expect(std::find(host.CommandNames().begin(), host.CommandNames().end(), "evil.run") ==
             host.CommandNames().end(),
         "project-local plugin commands must not register");
}

void TestPluginHostReloadIgnoresLateProjectLocalPlugin() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path user_plugins = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";

  WriteFile(project_root / "README.md", "late project plugin fixture\n");
  WritePluginInit(
      user_plugins, "good",
      R"(local ide = require("microide")
return ide.plugin({
  id = "user.good",
  setup = function(ctx)
    ctx.commands.add("good.echo", function() end)
  end
})
)");

  ScopedPluginConfigHomeEnv config_env(config_home);

  PluginHost host;
  host.SetCallbacks(MakePluginHostCallbacks());

  Expect(host.Reload(project_root), "initial plugin reload should succeed");
  Expect(host.LoadedPluginCount() == 1, "user plugin should load on first reload");

  const std::filesystem::path project_plugins = project_root / ".microide" / "plugins";
  WritePluginInit(
      project_plugins, "evil",
      R"(local ide = require("microide")
return ide.plugin({
  id = "repo.evil",
  setup = function(ctx)
    ctx.commands.add("evil.run", function() end)
  end
})
)");

  Expect(host.Reload(project_root), "plugins-reload path should ignore late project-local plugins");
  Expect(host.LoadedPluginCount() == 1,
         "reload should keep only the user config plugin loaded");
  Expect(host.CommandNames().size() == 1 && host.CommandNames().front() == "good.echo",
         "reload should not pick up project-local plugin commands");
}

void TestPluginAssetMonitorWatchesUserPluginRootOnly() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path user_plugins = config_home / "microide" / "plugins";
  WriteFile(user_plugins / ".keep", "");

  ScopedPluginConfigHomeEnv config_env(config_home);

  workspace::WorkspacePluginAssetMonitor monitor;
  monitor.SetProjectRoot(project_root);

  const std::vector<std::filesystem::path>& watched = monitor.WatchedRoots();
  Expect(watched.size() == 1, "plugin asset monitor should watch exactly one root");
  Expect(watched.front() == plugin::ResolveUserPluginInstallRoot(),
         "plugin asset monitor should watch the user config plugin directory");
  Expect(watched.front() != project_root,
         "plugin asset monitor must not watch the active project root");
  Expect(watched.front() != project_root / ".microide" / "plugins",
         "plugin asset monitor must not watch project-local plugin directories");
}

void TestPluginHostRejectsDuplicatePluginIds() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path global_plugins = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  WritePluginInit(
      global_plugins, "dup-a",
      R"(local ide = require("microide")
return ide.plugin({
  id = "dup",
  setup = function(ctx)
    ctx.commands.add("dup.global", function(ctx, args) end)
  end
})
)");
  WritePluginInit(
      global_plugins, "dup-b",
      R"(local ide = require("microide")
return ide.plugin({
  id = "dup",
  setup = function(ctx)
    ctx.commands.add("dup.project", function(ctx, args) end)
  end
})
)");

  ScopedPluginConfigHomeEnv config_env(config_home);

  std::vector<std::string> sink_errors;
  PluginHost host;
  auto callbacks = MakePluginHostCallbacks();
  callbacks.error_sink = [&](const std::string& error) { sink_errors.push_back(error); };
  host.SetCallbacks(std::move(callbacks));

  Expect(!host.Reload(project_root),
         "plugin reload should report failure when duplicate ids are discovered");
  Expect(host.LoadedPluginCount() == 1,
         "duplicate plugin ids should keep the first loaded plugin and reject the later one");
  Expect(host.Errors().size() == 1 &&
             host.Errors().front().find("duplicate plugin id 'dup'") != std::string::npos,
         "duplicate plugin ids should produce a clear error");
  Expect(sink_errors.size() == 1 &&
             sink_errors.front().find("duplicate plugin id 'dup'") != std::string::npos,
         "duplicate plugin ids should also be forwarded to the host error sink");
}

void TestPluginHostSkipsBackupPluginDirectories() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path global_plugins = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";

  WriteFile(project_root / "README.md", "plugin host backup fixture\n");
  WritePluginInit(
      global_plugins, "sample",
      R"(local ide = require("microide")
return ide.plugin({
  id = "sample",
  setup = function(ctx)
    ctx.commands.add("sample.run", function() end)
  end
})
)");
  WritePluginInit(
      global_plugins, "sample.bak-20260424",
      R"(local ide = require("microide")
return ide.plugin({
  id = "sample",
  setup = function(ctx)
    ctx.commands.add("sample.backup", function() end)
  end
})
)");

  ScopedPluginConfigHomeEnv config_env(config_home);

  PluginHost host;
  host.SetCallbacks(MakePluginHostCallbacks());
  Expect(host.Reload(project_root), "plugin reload should skip backup plugin directories");
  Expect(host.LoadedPluginCount() == 1, "backup plugin directories should not be loaded");
  Expect(host.CommandNames().size() == 1 && host.CommandNames().front() == "sample.run",
         "backup plugin directories should not register commands");
}

void TestPluginHostLuaRuntimeMatchesDocumentedStdlib() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path global_plugins = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";

  WriteFile(project_root / "README.md", "plugin stdlib fixture\n");
  WritePluginInit(
      global_plugins, "stdlib-probe",
      R"(local ide = require("microide")
return ide.plugin({
  id = "stdlib.probe",
  setup = function(ctx)
    ctx.log("base:" .. tostring(type(assert) == "function"))
    ctx.log("table:" .. tostring(type(table.insert) == "function"))
    ctx.log("string:" .. tostring(type(string.gsub) == "function"))
    ctx.log("math:" .. tostring(type(math.max) == "function"))
    ctx.log("utf8:" .. tostring(type(utf8.len) == "function"))
    ctx.log("package:" .. tostring(type(package) == "table"))
    ctx.log("package_path:" .. tostring(type(package.path) == "string"))
    ctx.log("io_nil:" .. tostring(io == nil))
    ctx.log("os_nil:" .. tostring(os == nil))
  end
})
)");

  ScopedPluginConfigHomeEnv config_env(config_home);

  PluginHost host;
  host.SetCallbacks(MakePluginHostCallbacks());

  Expect(host.Reload(project_root), "plugin reload should succeed for stdlib probe");
  const std::vector<std::string>& messages = host.Messages();
  Expect(messages.size() == 9, "stdlib probe should emit one log per documented capability check");
  Expect(messages[0] == "stdlib.probe: base:true", "base stdlib should be exposed");
  Expect(messages[1] == "stdlib.probe: table:true", "table stdlib should be exposed");
  Expect(messages[2] == "stdlib.probe: string:true", "string stdlib should be exposed");
  Expect(messages[3] == "stdlib.probe: math:true", "math stdlib should be exposed");
  Expect(messages[4] == "stdlib.probe: utf8:true", "utf8 stdlib should be exposed");
  Expect(messages[5] == "stdlib.probe: package:true", "package stdlib should remain exposed");
  Expect(messages[6] == "stdlib.probe: package_path:true",
         "package.path should remain available for Lua module resolution");
  Expect(messages[7] == "stdlib.probe: io_nil:true", "io stdlib should stay unavailable");
  Expect(messages[8] == "stdlib.probe: os_nil:true", "os stdlib should stay unavailable");
  Expect(host.Errors().empty(), "stdlib probe should not report host errors");
}

void TestPluginHostWatchdogAbortsRunawayCall() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#else
  std::string create_error;
  std::unique_ptr<microide::plugin::LuaRuntime> runtime =
      microide::plugin::LuaRuntime::Create(&create_error);
  Expect(runtime != nullptr, "LuaRuntime should be creatable for the watchdog test");

  // Trip the watchdog quickly so the test stays fast; production uses a generous
  // default hang guard.
  runtime->set_call_budget(std::chrono::milliseconds(20));
  lua_State* state = runtime->state();

  // A finite, well-behaved call must still succeed under the watchdog.
  Expect(luaL_loadstring(state, "local x = 0 for i = 1, 1000 do x = x + i end return x") == LUA_OK,
         "finite chunk should compile");
  std::string ok_error;
  Expect(runtime->PCall(0, 1, &ok_error), "finite chunk should complete within the budget");
  lua_settop(state, 0);

  // A runaway loop must be aborted, not hang the (UI) thread forever.
  Expect(luaL_loadstring(state, "while true do end") == LUA_OK, "infinite chunk should compile");
  std::string runaway_error;
  const auto started = std::chrono::steady_clock::now();
  const bool result = runtime->PCall(0, 0, &runaway_error);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  lua_settop(state, 0);

  Expect(!result, "watchdog should abort a runaway plugin call");
  Expect(runaway_error.find("time budget") != std::string::npos,
         "watchdog error should explain the time-budget abort");
  Expect(elapsed < std::chrono::seconds(5), "watchdog should abort well before any real hang");
#endif
}

void TestPluginHostPluginsUseIsolatedLuaStates() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path global_plugins = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  WriteFile(project_root / "README.md", "plugin shared-state fixture\n");
  WritePluginInit(
      global_plugins, "a-writer",
      R"(local ide = require("microide")
return ide.plugin({
  id = "isolated.writer",
  setup = function(ctx)
    _G.__microide_shared_probe = "writer-visible"
    ctx.log("writer:set")
  end
})
)");
  WritePluginInit(
      global_plugins, "b-reader",
      R"(local ide = require("microide")
return ide.plugin({
  id = "isolated.reader",
  setup = function(ctx)
    ctx.log("reader:" .. tostring(_G.__microide_shared_probe))
    _G.__microide_shared_probe = nil
  end
})
)");

  ScopedPluginConfigHomeEnv config_env(config_home);

  PluginHost host;
  host.SetCallbacks(MakePluginHostCallbacks());

  Expect(host.Reload(project_root), "plugin reload should succeed for isolated-state probe");
  const std::vector<std::string>& messages = host.Messages();
  Expect(messages.size() == 2, "isolated-state probe should emit exactly two setup logs");
  Expect(messages[0] == "isolated.writer: writer:set",
         "global writer plugin should run before the reader");
  Expect(messages[1] == "isolated.reader: reader:nil",
         "each plugin should receive its own Lua state rather than shared globals");
  Expect(host.Errors().empty(), "isolated-state probe should not report host errors");
}

void TestPluginHostPhase2Apis() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path global_plugins = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  WriteFile(project_root / "README.md", "plugin host phase2\n");

  const char* plugin_script =
#if defined(_WIN32)
      R"WIN(local ide = require("microide")
return ide.plugin({
  id = "phase2",
  capabilities = { process = { exec = true } },
  setup = function(ctx)
    ctx.sidebar.add({
      id = "problems",
      label = "Problems",
      snapshot = function()
        return {
          { label = "README", detail = "2:3", path = "README.md", line = 2, column = 3 }
        }
      end,
      on_confirm = function(item)
        ctx.log("confirm:" .. item.path .. ":" .. tostring(item.line) .. ":" .. tostring(item.column))
      end
    })

    ctx.commands.add("phase2.probe", function(ctx, args)
      local readme = ctx.files.read_text("README.md") or "missing"
      local exists = ctx.files.exists("README.md")
      local wrote = ctx.files.write_text("notes.txt", "written by plugin\n")
      local cat = ctx.process.run({"cmd", "/c", "more"}, { stdin = "stdin payload\n", cwd = "." })
      local pwd = ctx.process.run({"cmd", "/c", "cd"}, { cwd = "." })
      local envset = ctx.process.run({"cmd", "/c", "echo|set /p=%PHASE2_SET_ENV%"}, {
        cwd = ".",
        env = { PHASE2_SET_ENV = "plugin-value" }
      })
      local envunset = ctx.process.run({"cmd", "/c",
        "if defined PHASE2_REMOVE_ENV (echo|set /p=set) else (echo|set /p=unset)"}, {
        cwd = ".",
        env = { PHASE2_REMOVE_ENV = false }
      })
      ctx.log("read:" .. readme)
      ctx.log("exists:" .. tostring(exists))
      ctx.log("wrote:" .. tostring(wrote))
      ctx.log("cat:" .. tostring(cat.exit_code) .. ":" .. cat.stdout)
      ctx.log("pwd:" .. tostring(pwd.exit_code) .. ":" .. pwd.stdout)
      ctx.log("envset:" .. tostring(envset.exit_code) .. ":" .. envset.stdout)
      ctx.log("envunset:" .. tostring(envunset.exit_code) .. ":" .. envunset.stdout)
      ctx.workspace.open_file("README.md", 2, 3)
      ctx.sidebar.show("problems")
    end)
  end
})
)WIN";
#else
      R"WIN(local ide = require("microide")
return ide.plugin({
  id = "phase2",
  capabilities = { process = { exec = true } },
  setup = function(ctx)
    ctx.sidebar.add({
      id = "problems",
      label = "Problems",
      snapshot = function()
        return {
          { label = "README", detail = "2:3", path = "README.md", line = 2, column = 3 }
        }
      end,
      on_confirm = function(item)
        ctx.log("confirm:" .. item.path .. ":" .. tostring(item.line) .. ":" .. tostring(item.column))
      end
    })

    ctx.commands.add("phase2.probe", function(ctx, args)
      local readme = ctx.files.read_text("README.md") or "missing"
      local exists = ctx.files.exists("README.md")
      local wrote = ctx.files.write_text("notes.txt", "written by plugin\n")
      local cat = ctx.process.run({"cat"}, { stdin = "stdin payload\n", cwd = "." })
      local pwd = ctx.process.run({"pwd"}, { cwd = "." })
      local envset = ctx.process.run({"sh", "-c", "printf '%s' \"$PHASE2_SET_ENV\""}, {
        cwd = ".",
        env = { PHASE2_SET_ENV = "plugin-value" }
      })
      local envunset = ctx.process.run({
        "sh", "-c",
        "if [ -n \"${PHASE2_REMOVE_ENV+x}\" ]; then printf set; else printf unset; fi"
      }, {
        cwd = ".",
        env = { PHASE2_REMOVE_ENV = false }
      })
      ctx.log("read:" .. readme)
      ctx.log("exists:" .. tostring(exists))
      ctx.log("wrote:" .. tostring(wrote))
      ctx.log("cat:" .. tostring(cat.exit_code) .. ":" .. cat.stdout)
      ctx.log("pwd:" .. tostring(pwd.exit_code) .. ":" .. pwd.stdout)
      ctx.log("envset:" .. tostring(envset.exit_code) .. ":" .. envset.stdout)
      ctx.log("envunset:" .. tostring(envunset.exit_code) .. ":" .. envunset.stdout)
      ctx.workspace.open_file("README.md", 2, 3)
      ctx.sidebar.show("problems")
    end)
  end
})
)WIN";
#endif

  WritePluginInit(global_plugins, "phase2", plugin_script);

  ScopedPluginConfigHomeEnv config_env(config_home);
  ScopedEnvVar phase2_remove_env("PHASE2_REMOVE_ENV", "outer");

  std::optional<PluginHost::OpenFileRequest> opened_file;
  std::string shown_sidebar;
  PluginHost host;
  auto callbacks = MakePluginHostCallbacks();
  callbacks.open_file = [&](const PluginHost::OpenFileRequest& request) {
    opened_file = request;
    return true;
  };
  callbacks.show_sidebar = [&](std::string_view id) {
    shown_sidebar = std::string(id);
    return true;
  };
  host.SetCallbacks(std::move(callbacks));

  Expect(host.Reload(project_root), "phase2 plugin fixture should reload successfully");
  Expect(host.SidebarProviders().size() == 1,
         "phase2 plugin fixture should register one sidebar provider");
  Expect(host.SidebarProviders()[0].id == "problems",
         "phase2 plugin fixture should expose the registered sidebar id");

  std::string command_error;
  Expect(host.ExecuteCommand("phase2.probe", {}, &command_error),
         "phase2 plugin command should execute");
  Expect(command_error.empty(), "successful phase2 plugin command should not set an error");
  Expect(opened_file.has_value() && opened_file->path == (project_root / "README.md").lexically_normal(),
         "workspace.open_file should resolve project-relative paths");
  Expect(opened_file.has_value() && opened_file->line == 2 && opened_file->column == 3,
         "workspace.open_file should forward requested line and column");
  Expect(shown_sidebar == "problems",
         "ctx.sidebar.show should call back into the host");
  Expect(std::filesystem::exists(project_root / "notes.txt"),
         "ctx.files.write_text should create project-relative files");
  Expect(ReadFile(project_root / "notes.txt") == "written by plugin\n",
         "ctx.files.write_text should persist the requested content");
  Expect(std::find(host.Messages().begin(), host.Messages().end(), "phase2: exists:true") !=
             host.Messages().end(),
         "ctx.files.exists should report existing project-relative files");
  const auto cat_message =
      std::find_if(host.Messages().begin(), host.Messages().end(), [](const std::string& message) {
        return message.rfind("phase2: cat:0:stdin payload", 0) == 0;
      });
  Expect(cat_message != host.Messages().end(),
         ([&]() {
           std::string dump = "ctx.process.run should capture stdout and stdin; messages:";
           for (const auto& message : host.Messages()) {
             dump += " [" + message + "]";
           }
           return dump;
         })());
  const auto pwd_message =
      std::find_if(host.Messages().begin(), host.Messages().end(), [](const std::string& message) {
        return message.rfind("phase2: pwd:0:", 0) == 0;
      });
  Expect(pwd_message != host.Messages().end() &&
             pwd_message->find(project_root.lexically_normal().string()) != std::string::npos,
         "ctx.process.run should honor cwd relative to the active project");
#if defined(_WIN32)
  Expect(std::find(host.Messages().begin(), host.Messages().end(),
                   "phase2: envset:1:plugin-value") != host.Messages().end(),
         "ctx.process.run should apply environment overrides");
  Expect(std::find(host.Messages().begin(), host.Messages().end(),
                   "phase2: envunset:1:unset") != host.Messages().end(),
         "ctx.process.run should allow clearing inherited environment variables");
#else
  Expect(std::find(host.Messages().begin(), host.Messages().end(),
                   "phase2: envset:0:plugin-value") != host.Messages().end(),
         "ctx.process.run should apply environment overrides");
  Expect(std::find(host.Messages().begin(), host.Messages().end(),
                   "phase2: envunset:0:unset") != host.Messages().end(),
         "ctx.process.run should allow clearing inherited environment variables");
#endif

  std::vector<PluginHost::SidebarItem> items;
  Expect(host.SnapshotSidebar("problems", &items, &command_error),
         "plugin host should snapshot registered sidebars");
  Expect(items.size() == 1 && items[0].label == "README",
         "plugin sidebar snapshots should return item rows");
  host.ClearMessages();
  Expect(host.ConfirmSidebarItem("problems", items[0], &command_error),
         "plugin host should dispatch sidebar confirm handlers");
  Expect(host.Messages().size() == 1 &&
             host.Messages().front() == "phase2: confirm:" +
                                         (project_root / "README.md").lexically_normal().generic_string() +
                                         ":2:3",
         "plugin sidebar confirm handlers should receive resolved item paths and locations");
}

void TestPluginHostPhase3DiagnosticsApis() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path global_plugins = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  WriteFile(project_root / "README.md", "phase3 diagnostics\n");

  WritePluginInit(
      global_plugins, "phase3-diagnostics",
      R"(local ide = require("microide")
return ide.plugin({
  id = "phase3-diagnostics",
  setup = function(ctx)
    ctx.diagnostics.publish("README.md", {
      {
        message = "startup diagnostic",
        line = 1,
        column = 1,
        end_column = 8,
        severity = "warning"
      }
    })

    ctx.commands.add("phase3.publish", function(ctx, args)
      ctx.diagnostics.publish("README.md", {
        {
          message = "runtime diagnostic",
          line = 1,
          column = 2,
          end_column = 5,
          severity = "error"
        }
      })
    end)

    ctx.commands.add("phase3.clear-file", function(ctx, args)
      ctx.diagnostics.clear("README.md")
    end)
  end
})
)");

  ScopedPluginConfigHomeEnv config_env(config_home);

  microide::editor::DiagnosticsStore diagnostics_store;
  PluginHost host;
  auto callbacks = MakePluginHostCallbacks();
  callbacks.publish_diagnostics =
      [&](std::string_view owner,
          const std::filesystem::path& path,
          std::vector<microide::editor::Diagnostic> diagnostics) {
        diagnostics_store.ReplaceForOwnerFile(owner, path, std::move(diagnostics));
      };
  callbacks.clear_file_diagnostics =
      [&](std::string_view owner, const std::filesystem::path& path) {
        diagnostics_store.ClearOwnerFile(owner, path);
      };
  callbacks.clear_owner_diagnostics =
      [&](std::string_view owner) { diagnostics_store.ClearOwner(owner); };
  host.SetCallbacks(std::move(callbacks));

  Expect(host.Reload(project_root), "phase3 diagnostics plugin should reload successfully");
  const auto* startup_diagnostics = diagnostics_store.FindByPath(project_root / "README.md");
  Expect(startup_diagnostics != nullptr && startup_diagnostics->size() == 1,
         "setup should be able to publish initial diagnostics");
  Expect(startup_diagnostics->front().message == "startup diagnostic" &&
             startup_diagnostics->front().severity == microide::editor::DiagnosticSeverity::Warning,
         "setup diagnostics should preserve message and severity");

  std::string command_error;
  Expect(host.ExecuteCommand("phase3.publish", {}, &command_error),
         "phase3 diagnostics publish command should execute");
  const auto* runtime_diagnostics = diagnostics_store.FindByPath(project_root / "README.md");
  Expect(runtime_diagnostics != nullptr && runtime_diagnostics->size() == 1,
         "runtime diagnostic publication should replace the owner's file diagnostics");
  Expect(runtime_diagnostics->front().message == "runtime diagnostic" &&
             runtime_diagnostics->front().range.start.column == 1 &&
             runtime_diagnostics->front().range.end.column == 4,
         "published diagnostics should be converted to zero-based host ranges");

  Expect(host.ExecuteCommand("phase3.clear-file", {}, &command_error),
         "phase3 diagnostics clear command should execute");
  Expect(diagnostics_store.FindByPath(project_root / "README.md") == nullptr,
         "ctx.diagnostics.clear(path) should clear one file's diagnostics");

  Expect(host.ExecuteCommand("phase3.publish", {}, &command_error),
         "phase3 diagnostics publish command should be reusable");
  host.Shutdown();
  Expect(diagnostics_store.FindByPath(project_root / "README.md") == nullptr,
         "plugin shutdown should clear the owner's diagnostics");
}

void TestPluginHostPhase2StatusApis() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path global_plugins = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  WriteFile(project_root / "README.md", "phase2 status\n");

  WritePluginInit(
      global_plugins, "phase2-status",
      R"(local ide = require("microide")
return ide.plugin({
  id = "phase2-status",
  setup = function(ctx)
    ctx.status.add({
      id = "counter",
      text = "0",
      tooltip = "Counter is 0",
      alignment = "right",
    })
    ctx.commands.add("phase2-status.tick", function(ctx, args)
      ctx.status.update("counter", {
        text = "1",
        tooltip = "Counter is 1",
      })
    end)
  end
})
)");

  ScopedPluginConfigHomeEnv config_env(config_home);

  int redraw_requests = 0;
  PluginHost host;
  auto callbacks = MakePluginHostCallbacks();
  callbacks.request_status_redraw = [&]() { ++redraw_requests; };
  host.SetCallbacks(std::move(callbacks));

  Expect(host.Reload(project_root), "phase2 status plugin should reload successfully");
  Expect(host.ContributedStatusItems().size() == 1,
         "phase2 status plugin should contribute one status item");
  Expect(host.ContributedStatusItems().front().text == "0",
         "status items should expose the initial text");

  std::string command_error;
  Expect(host.ExecuteCommand("phase2-status.tick", {}, &command_error),
         "phase2 status update command should execute");
  Expect(command_error.empty(), "successful status updates should not set an error");
  Expect(redraw_requests == 1, "status updates should request one chrome redraw");
  Expect(host.ContributedStatusItems().front().text == "1" &&
             host.ContributedStatusItems().front().tooltip == "Counter is 1",
         "status updates should mutate the exported status item state");
}

void TestPluginHostPhase3HoverApis() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path global_plugins = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  WriteFile(project_root / "README.md", "phase3 hover\n");

  WritePluginInit(
      global_plugins, "phase3-hover",
      R"(local ide = require("microide")
return ide.plugin({
  id = "phase3-hover",
  setup = function(ctx)
    ctx.hover.add({
      id = "phase3-hover.provider",
      provide = function(buffer, position)
        if buffer.relative_path == "README.md" and position.line == 1 and position.column == 3 then
          return {
            title = "Hover README",
            content = buffer.relative_path .. ":" .. tostring(position.line) .. ":" .. tostring(position.column)
          }
        end
        return nil
      end
    })
  end
})
)");

  ScopedPluginConfigHomeEnv config_env(config_home);

  PluginHost host;
  host.SetCallbacks(MakePluginHostCallbacks());

  Expect(host.Reload(project_root), "phase3 hover plugin should reload successfully");
  PluginHost::HoverResult hover;
  std::string hover_error;
  Expect(host.QueryHover(project_root / "README.md", 1, 3, &hover, &hover_error),
         "hover query should find a matching plugin-provided result");
  Expect(hover_error.empty(), "successful hover queries should not set an error");
  Expect(hover.title == "Hover README" && hover.content == "README.md:1:3",
         "hover queries should preserve the returned title and content");

  hover = PluginHost::HoverResult{};
  Expect(!host.QueryHover(project_root / "README.md", 2, 1, &hover, &hover_error),
         "hover query should report no result when providers return nil");
  Expect(hover_error.empty(), "missing hover results should not set an error");
  Expect(hover.title.empty() && hover.content.empty(),
         "missing hover results should leave the output empty");
}

void TestPluginHostPhase3RuntimeApis() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path global_plugins = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path source = project_root / "README.todo";
  WriteFile(source, "alpha\n");

  WritePluginInit(
      global_plugins, "phase3-runtime",
      R"lua(local ide = require("microide")
return ide.plugin({
  id = "phase3-runtime",
  setup = function(ctx)
    ctx.commands.add("phase3-runtime.echo", function(ctx, args)
      ctx.log("echo:" .. table.concat(args, ":"))
    end)

    ctx.save_participants.add("uppercase", function(buffer)
      return {
        text = string.upper(buffer.text)
      }
    end)

    ctx.completion.add({
      id = "todo",
      language_id = "todo",
      provide = function(buffer, position, trigger)
        return {
          {
            label = "TODO",
            detail = buffer.relative_path,
            documentation = "trigger:" .. trigger,
            insert_text = "TODO()"
          }
        }
      end
    })

    ctx.code_actions.add({
      id = "todo",
      language_id = "todo",
      provide = function(buffer, range)
        return {
          {
            title = "Log quick fix",
            command = "phase3-runtime.echo",
            arguments = {
              buffer.relative_path,
              tostring(range.start.line),
              tostring(range['end'].column)
            }
          }
        }
      end
    })

    ctx.tests.add({
      id = "todo",
      language_id = "todo",
      discover = function(buffer)
        return {
          {
            id = "todo.case",
            label = "TODO case",
            file = buffer.relative_path,
            line = 3
          }
        }
      end,
      run = function(test_ids)
        return {
          {
            test_id = test_ids[1],
            state = "passed",
            message = "ok",
            duration_ms = 12
          }
        }
      end
    })
  end
})
)lua");

  ScopedPluginConfigHomeEnv config_env(config_home);

  PluginHost host;
  host.SetCallbacks(MakePluginHostCallbacks());

  Expect(host.Reload(project_root), "phase3 runtime plugin should reload successfully");

  std::string text = "alpha\nbeta\n";
  std::string runtime_error;
  Expect(host.RunSaveParticipants(source, &text, &runtime_error),
         "save participant runtime should execute");
  Expect(runtime_error.empty(), "successful save participant execution should not set an error");
  Expect(text == "ALPHA\nBETA\n", "save participants should be able to replace buffer text");

  const auto completions = host.QueryCompletions("todo", source, 1, 3, ".", &runtime_error);
  Expect(runtime_error.empty(), "successful completion queries should not set an error");
  Expect(completions.size() == 1, "completion query should return one runtime result");
  Expect(completions.front().label == "TODO" &&
             completions.front().insert_text == "TODO()" &&
             completions.front().detail == "README.todo",
         "completion providers should preserve label, insert text, and relative file context");

  const auto actions = host.QueryCodeActions("todo", source, 1, 2, 1, 5, &runtime_error);
  Expect(runtime_error.empty(), "successful code action queries should not set an error");
  Expect(actions.size() == 1, "code action query should return one runtime result");
  Expect(actions.front().title == "Log quick fix" &&
             actions.front().command == "phase3-runtime.echo" &&
             actions.front().arguments == std::vector<std::string>{"README.todo", "1", "5"},
         "code action providers should preserve returned commands and arguments");

  std::vector<PluginHost::TestCase> discovered_tests;
  Expect(host.DiscoverTests("phase3-runtime.todo", source, &discovered_tests, &runtime_error),
         "test discovery should execute the provider runtime");
  Expect(runtime_error.empty(), "successful test discovery should not set an error");
  Expect(discovered_tests.size() == 1, "test discovery should return one test case");
  Expect(discovered_tests.front().id == "todo.case" &&
             discovered_tests.front().file == source.lexically_normal() &&
             discovered_tests.front().line == 3,
         "test discovery should resolve file paths against the active project");

  std::vector<PluginHost::TestRunResult> run_results;
  Expect(host.RunTests("phase3-runtime.todo", {"todo.case"}, &run_results, &runtime_error),
         "test execution should execute the provider runtime");
  Expect(runtime_error.empty(), "successful test execution should not set an error");
  Expect(run_results.size() == 1, "test execution should return one test result");
  Expect(run_results.front().test_id == "todo.case" &&
             run_results.front().state == "passed" &&
             run_results.front().duration_ms == 12,
         "test execution should preserve the returned test result fields");
}

void TestPluginHostPhase4ContributionApis() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path global_plugins = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  WriteFile(project_root / "README.md", "phase4 contributions\n");

  WritePluginInit(
      global_plugins, "phase4-contrib",
      R"(local ide = require("microide")
return ide.plugin({
  id = "phase4-contrib",
  setup = function(ctx)
    ctx.scm.add("sample", "Sample SCM")
    ctx.annotations.add({
      id = "blame",
      label = "Plugin Blame",
      type = "blame",
      language_id = "todo"
    })
    ctx.auth.add("github", "GitHub")
  end
})
)");

  ScopedPluginConfigHomeEnv config_env(config_home);

  PluginHost host;
  host.SetCallbacks(MakePluginHostCallbacks());

  Expect(host.Reload(project_root), "phase4 contribution plugin should reload successfully");
  Expect(host.ContributedScmProviders().size() == 1,
         "phase4 contribution plugin should register one SCM provider");
  Expect(host.ContributedScmProviders().front().id == "phase4-contrib.sample",
         "SCM providers should be plugin-prefixed");
  Expect(host.ContributedAnnotationProviders().size() == 1 &&
             host.ContributedAnnotationProviders().front().type == "blame" &&
             host.ContributedAnnotationProviders().front().language_id == "todo",
         "annotation providers should preserve type and language metadata");
  Expect(host.ContributedAuthProviders().size() == 1 &&
             host.ContributedAuthProviders().front().id == "phase4-contrib.github",
         "auth providers should be registered with plugin-prefixed ids");
}

void TestPluginHostPhase5WorkspaceApis() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path global_plugins = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  WriteFile(project_root / "README.md", "phase5 workspace\n");

  WritePluginInit(
      global_plugins, "phase5-workspace",
      R"(local ide = require("microide")
return ide.plugin({
  id = "phase5-workspace",
  setup = function(ctx)
    ctx.commands.add("phase5.probe-active-buffer", function(ctx, args)
      local buffer = ctx.workspace.active_buffer()
      if buffer == nil then
        ctx.log("active:nil")
        return
      end
      ctx.log("active:" .. buffer.relative_path .. ":" .. tostring(buffer.line) .. ":" .. tostring(buffer.column))
    end)
  end
})
)");

  ScopedPluginConfigHomeEnv config_env(config_home);

  PluginHost host;
  auto callbacks = MakePluginHostCallbacks();
  callbacks.active_buffer = [&]() -> std::optional<PluginHost::ActiveBuffer> {
    return PluginHost::ActiveBuffer{
        .path = (project_root / "README.md").lexically_normal(),
        .line = 2,
        .column = 5,
    };
  };
  host.SetCallbacks(std::move(callbacks));

  Expect(host.Reload(project_root), "phase5 workspace plugin should reload successfully");
  std::string command_error;
  Expect(host.ExecuteCommand("phase5.probe-active-buffer", {}, &command_error),
         ("phase5 workspace command should execute: " + command_error).c_str());
  Expect(command_error.empty(), "successful phase5 workspace command should not set an error");
  Expect(!host.Messages().empty() &&
             host.Messages().back() == "phase5-workspace: active:README.md:2:5",
         "ctx.workspace.active_buffer should expose the active relative path and one-based cursor");
}

void TestPluginHostPhase5LspApis() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path global_plugins = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  WriteFile(project_root / "README.md", "phase5 lsp\n");

  WritePluginInit(
      global_plugins, "phase5-lsp",
      R"(local ide = require("microide")
return ide.plugin({
  id = "phase5-lsp",
  capabilities = { process = { exec = true } },
  setup = function(ctx)
    ctx.lsp.add({
      id = "markdown",
      language_id = "markdown",
      command = { "python3", "fake-lsp.py" }
    })
    ctx.lsp.add({
      id = "clike",
      language_ids = { "c", "c++", "objective-c" },
      command = { "clangd" }
    })
  end
})
)");

  ScopedPluginConfigHomeEnv config_env(config_home);

  PluginHost host;
  host.SetCallbacks(MakePluginHostCallbacks());

  Expect(host.Reload(project_root), "phase5 lsp plugin should reload successfully");
  Expect(host.ContributedLanguageServers().size() == 2,
         "ctx.lsp.add should register both language servers");
  const auto& markdown_server = host.ContributedLanguageServers().front();
  Expect(markdown_server.id == "phase5-lsp.markdown" &&
             markdown_server.language_ids == std::vector<std::string>{"markdown"} &&
             markdown_server.command.size() == 2 && markdown_server.command.front() == "python3",
         "a single language_id should fold into a one-element language_ids list");
  const auto& clike_server = host.ContributedLanguageServers().back();
  Expect(clike_server.id == "phase5-lsp.clike" &&
             clike_server.language_ids ==
                 std::vector<std::string>{"c", "c++", "objective-c"} &&
             clike_server.command == std::vector<std::string>{"clangd"},
         "a language_ids array should be preserved in order for one shared server");
}

void TestPluginHostDebugAdapterRegistration() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path global_plugins = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  WriteFile(project_root / "README.md", "debug adapter fixture\n");

  WritePluginInit(
      global_plugins, "phase1-debug",
      R"(local ide = require("microide")
return ide.plugin({
  id = "phase1-debug",
  capabilities = { process = { exec = true } },
  setup = function(ctx)
    ctx.debug.add({
      id = "debugpy",
      type = "debugpy",
      command = { "python3", "-m", "debugpy.adapter" }
    })
    ctx.debug.add({
      id = "implicit-type",
      command = { "lldb-dap" }
    })
  end
})
)");

  ScopedPluginConfigHomeEnv config_env(config_home);

  PluginHost host;
  host.SetCallbacks(MakePluginHostCallbacks());

  Expect(host.Reload(project_root), "debug adapter plugin should reload successfully");
  Expect(host.ContributedDebugAdapters().size() == 2,
         "ctx.debug.add should register both debug adapters");

  const auto& debugpy = host.ContributedDebugAdapters().front();
  Expect(debugpy.id == "phase1-debug.debugpy" && debugpy.type == "debugpy" &&
             debugpy.command == std::vector<std::string>{"python3", "-m", "debugpy.adapter"} &&
             debugpy.plugin_id == "phase1-debug",
         "an explicit type should be preserved with the namespaced id");

  const auto& implicit = host.ContributedDebugAdapters().back();
  Expect(implicit.id == "phase1-debug.implicit-type" && implicit.type == "implicit-type" &&
             implicit.command == std::vector<std::string>{"lldb-dap"},
         "an omitted type should default to the local id");
}

void TestPluginHostDebugAdapterRequiresProcessExec() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path global_plugins = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  WriteFile(project_root / "README.md", "debug adapter capability fixture\n");

  // No capabilities.process.exec: ctx.debug.add must be rejected like ctx.lsp.add.
  WritePluginInit(
      global_plugins, "phase1-debug-nocap",
      R"(local ide = require("microide")
return ide.plugin({
  id = "phase1-debug-nocap",
  setup = function(ctx)
    ctx.debug.add({ id = "debugpy", type = "debugpy", command = { "python3" } })
  end
})
)");

  ScopedPluginConfigHomeEnv config_env(config_home);

  PluginHost host;
  host.SetCallbacks(MakePluginHostCallbacks());

  host.Reload(project_root);
  Expect(host.ContributedDebugAdapters().empty(),
         "ctx.debug.add without capabilities.process.exec should register nothing");
}

void TestPluginHostLaunchConfigRegistration() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path global_plugins = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  WriteFile(project_root / "README.md", "launch config fixture\n");

  WritePluginInit(
      global_plugins, "phase2-config",
      R"(local ide = require("microide")
return ide.plugin({
  id = "phase2-config",
  capabilities = { process = { exec = true } },
  setup = function(ctx)
    ctx.debug.addConfig({
      id = "main",
      name = "Debug main.py",
      type = "debugpy",
      request = "launch",
      arguments = '{"program":"main.py","stopOnEntry":true}'
    })
    ctx.debug.addConfig({
      id = "attach-default",
      type = "debugpy"
    })
  end
})
)");

  ScopedPluginConfigHomeEnv config_env(config_home);

  PluginHost host;
  host.SetCallbacks(MakePluginHostCallbacks());

  Expect(host.Reload(project_root), "launch-config plugin should reload successfully");
  Expect(host.ContributedLaunchConfigs().size() == 2,
         "ctx.debug.addConfig should register both launch configs");

  const auto& main_config = host.ContributedLaunchConfigs().front();
  Expect(main_config.id == "phase2-config.main" && main_config.name == "Debug main.py" &&
             main_config.type == "debugpy" && main_config.request == "launch" &&
             main_config.arguments_json == R"({"program":"main.py","stopOnEntry":true})" &&
             main_config.plugin_id == "phase2-config",
         "an explicit launch config should round-trip with namespaced id and verbatim arguments");

  const auto& defaulted = host.ContributedLaunchConfigs().back();
  Expect(defaulted.id == "phase2-config.attach-default" && defaulted.name == "attach-default" &&
             defaulted.request == "launch",
         "an omitted name should default to the id and request to launch");
}

void TestPluginHostLaunchConfigRequiresProcessExec() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path global_plugins = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  WriteFile(project_root / "README.md", "launch config capability fixture\n");

  WritePluginInit(
      global_plugins, "phase2-config-nocap",
      R"(local ide = require("microide")
return ide.plugin({
  id = "phase2-config-nocap",
  setup = function(ctx)
    ctx.debug.addConfig({ id = "main", type = "debugpy" })
  end
})
)");

  ScopedPluginConfigHomeEnv config_env(config_home);

  PluginHost host;
  host.SetCallbacks(MakePluginHostCallbacks());

  host.Reload(project_root);
  Expect(host.ContributedLaunchConfigs().empty(),
         "ctx.debug.addConfig without capabilities.process.exec should register nothing");
}

void TestRepoTypescriptLspPluginUsesAbsoluteProjectBinary() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path global_plugins = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  WriteFile(project_root / "README.md", "typescript plugin fixture\n");
  WriteFile(project_root / "node_modules" / ".bin" / "typescript-language-server", "#!/bin/sh\n");

  CopyRepoPlugin(global_plugins, "typescript-lsp");

  ScopedPluginConfigHomeEnv config_env(config_home);

  PluginHost host;
  host.SetCallbacks(MakePluginHostCallbacks());

  Expect(host.Reload(project_root), "repo typescript-lsp plugin should reload successfully");
  Expect(host.ContributedLanguageServers().size() == 1,
         "repo typescript-lsp plugin should contribute one language server");

  const auto& server = host.ContributedLanguageServers().front();
  Expect(server.id == "typescript-lsp.typescript" &&
             server.language_ids == std::vector<std::string>{"typescript"},
         "repo typescript-lsp plugin should preserve its ids");
  Expect(server.command.size() == 2 &&
             server.command.front() ==
                 (project_root / "node_modules" / ".bin" / "typescript-language-server").generic_string() &&
             server.command.back() == "--stdio",
         "repo typescript-lsp plugin should use an absolute project-local language server path");
}

void TestRepoCppLspPluginRegistersClangdForCLikeLanguages() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path global_plugins = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  WriteFile(project_root / "README.md", "cpp plugin fixture\n");

  CopyRepoPlugin(global_plugins, "cpp-lsp");

  ScopedPluginConfigHomeEnv config_env(config_home);

  PluginHost host;
  host.SetCallbacks(MakePluginHostCallbacks());

  Expect(host.Reload(project_root), "repo cpp-lsp plugin should reload successfully");
  Expect(host.ContributedLanguageServers().size() == 1,
         "repo cpp-lsp plugin should contribute one shared language server");

  const auto& server = host.ContributedLanguageServers().front();
  Expect(server.id == "cpp-lsp.clangd" &&
             server.language_ids == std::vector<std::string>{"c", "c++", "objective-c"},
         "repo cpp-lsp plugin should register clangd for c / c++ / objective-c as one server");
  Expect(!server.command.empty() && server.command.front() == "clangd",
         "repo cpp-lsp plugin should launch the clangd binary by default");
}

void TestPluginHostRunAsyncInvokesCallbackSynchronously() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path global_plugins = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path source = project_root / "src" / "main.js";
  WriteFile(project_root / "README.md", "async fixture\n");
  WriteFile(source, "console.log('hello');\n");

  // run_async now runs on the plugin worker thread, where blocking on the
  // subprocess never stalls the UI, and invokes its callback synchronously on that
  // same thread (the lua_State's owner). The legacy detached-thread + UI-thread
  // callback machinery is gone, so the callback has already fired by the time the
  // triggering event returns.
  WritePluginInit(
      global_plugins, "async-run",
      R"(local ide = require("microide")
return ide.plugin({
  id = "async.run",
  capabilities = { process = { exec = true } },
  on_buffer_open = function(ctx, buffer)
    ctx.process.run_async({"sh", "-lc", "printf done"}, nil, function(result)
      ctx.log("async-complete:" .. tostring(result.exit_code))
    end)
  end
})
)");

  ScopedPluginConfigHomeEnv config_env(config_home);

  PluginHost host;
  host.SetCallbacks(MakePluginHostCallbacks());

  Expect(host.Reload(project_root), "async fixture should load");
  // No worker is wired in this harness, so the event runs inline; the callback has
  // therefore already fired once OnBufferOpen returns.
  host.OnBufferOpen(source);
  Expect(std::any_of(host.Messages().begin(), host.Messages().end(),
                     [](const std::string& entry) {
                       return entry == "async.run: async-complete:0";
                     }),
         "run_async should invoke its callback synchronously with the subprocess result");
  Expect(host.Errors().empty(), "a successful run_async callback should not surface errors");

  // Reloading and shutting down after a synchronous run_async must stay clean.
  Expect(host.Reload(project_root), "reload after run_async should succeed");
  host.Shutdown();
  Expect(host.Errors().empty(), "reload/shutdown after run_async should not surface errors");
}

// Exercises the process.run argument-validation error paths. These raise a Lua
// error from inside a C function that has already built std::vector/std::string
// locals; raising Lua errors is a C longjmp, so the fix defers the raise until
// those locals have destructed (see src/plugin/LuaError.h). The test asserts the
// error message is preserved AND that a subsequent valid call still works, which
// would fail if the deferred-raise restructure left the Lua stack desynchronized.
// Under the ASAN preset it also proves the longjmp no longer leaks the locals.
void TestPluginHostProcessRunReportsArgumentErrorsWithoutCorruptingState() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path global_plugins = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  WriteFile(project_root / "README.md", "process run error fixture\n");

  WritePluginInit(
      global_plugins, "proc-errors",
      R"(local ide = require("microide")
return ide.plugin({
  id = "proc.errors",
  capabilities = { process = { exec = true } },
  setup = function(ctx)
    ctx.commands.add("proc.bad-argv", function(ctx, args)
      ctx.process.run({ {} })
    end)
    ctx.commands.add("proc.bad-env-value", function(ctx, args)
      ctx.process.run({"true"}, { env = { BAD = {} } })
    end)
    ctx.commands.add("diag.bad-path", function(ctx, args)
      ctx.diagnostics.publish({}, {})
    end)
    ctx.commands.add("files.bad-arg", function(ctx, args)
      ctx.files.read_text({})
    end)
    ctx.commands.add("ws.bad-line", function(ctx, args)
      ctx.workspace.open_file("README.md", "not-a-number")
    end)
    ctx.commands.add("proc.ok", function(ctx, args)
      local result = ctx.process.run({"true"})
      ctx.log("proc-ok:" .. tostring(result.exit_code))
    end)
  end
})
)");

  ScopedPluginConfigHomeEnv config_env(config_home);
  PluginHost host;
  host.SetCallbacks(MakePluginHostCallbacks());
  Expect(host.Reload(project_root), "process-error plugin should load");

  std::string command_error;
  Expect(!host.ExecuteCommand("proc.bad-argv", {}, &command_error),
         "non-string argv entries should fail the command");
  Expect(command_error.find("process argv entries must be strings") != std::string::npos,
         "bad argv should preserve the descriptive error message");

  command_error.clear();
  Expect(!host.ExecuteCommand("proc.bad-env-value", {}, &command_error),
         "non-string/non-false env values should fail the command");
  Expect(command_error.find("process env values must be strings or false") != std::string::npos,
         "bad env value should preserve the descriptive error message");

  // The remaining cases exercise the other delegating wrappers (diagnostics /
  // files / workspace), whose null-host ternary fallbacks previously materialized
  // a std::filesystem::path or Callbacks temporary that the inner longjmp leaked.
  command_error.clear();
  Expect(!host.ExecuteCommand("diag.bad-path", {}, &command_error),
         "diagnostics.publish with a non-string path should fail the command");
  command_error.clear();
  Expect(!host.ExecuteCommand("files.bad-arg", {}, &command_error),
         "files.read_text with a non-string argument should fail the command");
  command_error.clear();
  Expect(!host.ExecuteCommand("ws.bad-line", {}, &command_error),
         "workspace.open_file with a non-numeric line should fail the command");

  host.ClearMessages();
  command_error.clear();
  Expect(host.ExecuteCommand("proc.ok", {}, &command_error),
         "a valid process.run must still succeed after earlier error paths");
  Expect(command_error.empty(), "successful command should clear the error output");
  Expect(!host.Messages().empty() && host.Messages().back() == "proc.errors: proc-ok:0",
         "the Lua stack must remain intact across deferred-raise error paths");
}

// Locks the batched ordered-view rebuild: registration only flips a dirty bit, so a
// plugin registering several commands out of order must still expose a single fully
// sorted, deduplicated CommandNames() view after load, merged across plugins.
void TestPluginHostBatchedCommandRegistrationSortsOnce() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path global_plugins = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  WriteFile(project_root / "README.md", "batch fixture\n");

  WritePluginInit(
      global_plugins, "batch-one",
      R"(local ide = require("microide")
return ide.plugin({
  id = "batch.one",
  setup = function(ctx)
    ctx.commands.add("batch.zeta", function() end)
    ctx.commands.add("batch.alpha", function() end)
    ctx.commands.add("batch.mid", function() end)
  end,
})
)");
  WritePluginInit(
      global_plugins, "batch-two",
      R"(local ide = require("microide")
return ide.plugin({
  id = "batch.two",
  setup = function(ctx)
    ctx.commands.add("batch.beta", function() end)
  end,
})
)");

  ScopedPluginConfigHomeEnv config_env(config_home);

  PluginHost host;
  host.SetCallbacks(MakePluginHostCallbacks());
  Expect(host.Reload(project_root), "plugin reload should succeed for batch fixture");

  const std::vector<std::string>& names = host.CommandNames();
  const std::vector<std::string> expected = {"batch.alpha", "batch.beta", "batch.mid", "batch.zeta"};
  Expect(names.size() == expected.size(),
         "batched registration should expose every command exactly once");
  Expect(names == expected,
         "command names should be globally sorted and deduplicated across plugins");
  Expect(std::is_sorted(names.begin(), names.end()),
         "command names view must remain sorted after lazy rebuild");

  // A second read without intervening registration must return the identical view
  // (the dirty flag is cleared after the first rebuild).
  Expect(host.CommandNames() == expected, "repeated reads must return a stable sorted view");
}

// A plugin command that returns a string should surface it as host feedback; one
// that returns nothing leaves feedback empty (the host then synthesizes a default).
void TestPluginHostCommandReturnsFeedback() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path global_plugins = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  WriteFile(project_root / "README.md", "feedback fixture\n");

  WritePluginInit(
      global_plugins, "feedback-sample",
      R"(local ide = require("microide")
return ide.plugin({
  id = "feedback.sample",
  setup = function(ctx)
    ctx.commands.add("feedback.echo", function(ctx, args)
      return "echoed " .. table.concat(args, ",")
    end)
    ctx.commands.add("feedback.silent", function() end)
  end,
})
)");

  ScopedPluginConfigHomeEnv config_env(config_home);
  PluginHost host;
  host.SetCallbacks(MakePluginHostCallbacks());
  Expect(host.Reload(project_root), "plugin reload should succeed for feedback fixture");

  std::string error;
  std::string feedback;
  Expect(host.ExecuteCommand("feedback.echo", {"a", "b"}, &error, &feedback),
         "returning command should execute");
  Expect(feedback == "echoed a,b",
         "command string return value should be surfaced as feedback");

  feedback = "stale";
  Expect(host.ExecuteCommand("feedback.silent", {}, &error, &feedback),
         "silent command should execute");
  Expect(feedback.empty(), "a command returning nothing should leave feedback empty");
}

// microide.notify(level, message) should route to the host show_notification callback.
void TestPluginHostNotifyInvokesCallback() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path global_plugins = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  WriteFile(project_root / "README.md", "notify fixture\n");

  WritePluginInit(
      global_plugins, "notify-sample",
      R"(local ide = require("microide")
return ide.plugin({
  id = "notify.sample",
  setup = function(ctx)
    ctx.notify("warning", "heads up")
  end,
})
)");

  ScopedPluginConfigHomeEnv config_env(config_home);
  std::vector<std::pair<std::string, std::string>> notifications;
  PluginHost host;
  auto callbacks = MakePluginHostCallbacks();
  callbacks.show_notification = [&](const std::string& level, const std::string& message) {
    notifications.emplace_back(level, message);
  };
  host.SetCallbacks(std::move(callbacks));
  Expect(host.Reload(project_root), "plugin reload should succeed for notify fixture");

  Expect(notifications.size() == 1, "ctx.notify should invoke the host callback exactly once");
  Expect(notifications.front().first == "warning" && notifications.front().second == "heads up",
         "ctx.notify should forward the level and message verbatim");
}

// A disabled plugin id should skip setup (no commands) but still be listed as disabled,
// and re-enabling it should load it on the next reload.
void TestPluginHostDisabledPluginsSkipSetupButRemainListed() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path global_plugins = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  WriteFile(project_root / "README.md", "disable fixture\n");

  WritePluginInit(global_plugins, "alpha-sample",
                  R"(local ide = require("microide")
return ide.plugin({
  id = "alpha.sample",
  setup = function(ctx) ctx.commands.add("alpha.cmd", function() end) end,
})
)");
  WritePluginInit(global_plugins, "beta-sample",
                  R"(local ide = require("microide")
return ide.plugin({
  id = "beta.sample",
  setup = function(ctx) ctx.commands.add("beta.cmd", function() end) end,
})
)");

  ScopedPluginConfigHomeEnv config_env(config_home);
  PluginHost host;
  host.SetCallbacks(MakePluginHostCallbacks());

  host.SetDisabledPlugins({"beta.sample"});
  Expect(host.Reload(project_root), "reload should succeed with one plugin disabled");
  Expect(host.LoadedPluginCount() == 1, "only the enabled plugin should be fully loaded");
  Expect(std::find(host.CommandNames().begin(), host.CommandNames().end(), "alpha.cmd") !=
             host.CommandNames().end(),
         "the enabled plugin's command should be registered");
  Expect(std::find(host.CommandNames().begin(), host.CommandNames().end(), "beta.cmd") ==
             host.CommandNames().end(),
         "the disabled plugin's command must not be registered");

  const std::vector<PluginHost::LoadedPlugin> listed = host.LoadedPlugins();
  Expect(listed.size() == 2, "both plugins should be listed (enabled and disabled)");
  Expect(listed[0].id == "alpha.sample" && listed[0].enabled,
         "alpha should be listed first and enabled");
  Expect(listed[1].id == "beta.sample" && !listed[1].enabled,
         "beta should be listed as disabled");

  host.SetDisabledPlugins({});
  Expect(host.Reload(project_root), "reload should succeed after re-enabling");
  Expect(host.LoadedPluginCount() == 2, "re-enabling should load both plugins");
  Expect(std::find(host.CommandNames().begin(), host.CommandNames().end(), "beta.cmd") !=
             host.CommandNames().end(),
         "re-enabled plugin's command should register after reload");
}

// Default-posture plugin (no capabilities declared): filesystem access is project-scoped and
// process execution is denied. Reads/writes inside the project succeed; absolute paths and `..`
// escapes are refused (falsy result + a denial diagnostic), and process.run raises.
void TestPluginHostFilesystemSandbox() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path global_plugins = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  WriteFile(project_root / "README.md", "sandbox fixture\n");
  WriteFile(temp_dir.path() / "secret.txt", "top secret\n");

  WritePluginInit(
      global_plugins, "fs-sandbox",
      R"(local ide = require("microide")
return ide.plugin({
  id = "fs.sandbox",
  setup = function(ctx)
    ctx.commands.add("probe.fs", function(ctx, args)
      ctx.log("read-project:" .. tostring(ctx.files.read_text("README.md") ~= nil))
      ctx.log("read-escape:" .. tostring(ctx.files.read_text("../secret.txt")))
      ctx.log("read-absolute:" .. tostring(ctx.files.read_text("/etc/hostname")))
      ctx.log("write-project:" .. tostring(ctx.files.write_text("out.txt", "x")))
      ctx.log("write-escape:" .. tostring(ctx.files.write_text("../evil.txt", "x")))
    end)
    ctx.commands.add("probe.proc", function(ctx, args)
      ctx.process.run({"true"})
    end)
  end,
})
)");

  ScopedPluginConfigHomeEnv config_env(config_home);
  PluginHost host;
  host.SetCallbacks(MakePluginHostCallbacks());
  Expect(host.Reload(project_root), "fs sandbox plugin should load");

  std::string error;
  Expect(host.ExecuteCommand("probe.fs", {}, &error), "fs probe command should execute");

  const auto has_message = [&](std::string_view needle) {
    for (const std::string& message : host.Messages()) {
      if (message.find(needle) != std::string::npos) {
        return true;
      }
    }
    return false;
  };
  Expect(has_message("read-project:true"), "reading a project file should be permitted");
  Expect(has_message("read-escape:nil"), "reading via .. escape should be denied (nil)");
  Expect(has_message("read-absolute:nil"), "reading an absolute system path should be denied (nil)");
  Expect(has_message("write-project:true"), "writing inside the project should be permitted");
  Expect(has_message("write-escape:false"), "writing via .. escape should be denied (false)");
  Expect(std::filesystem::exists(project_root / "out.txt"),
         "the permitted write should reach disk inside the project");
  Expect(!std::filesystem::exists(temp_dir.path() / "evil.txt"),
         "the denied write must not escape the project");
  Expect(has_message("denied files.read_text"), "a denial diagnostic should name the refused read");
  Expect(has_message("denied files.write_text"), "a denial diagnostic should name the refused write");

  error.clear();
  Expect(!host.ExecuteCommand("probe.proc", {}, &error),
         "process.run without the process capability must fail the command");
  Expect(error.find("process execution not permitted") != std::string::npos,
         "the process denial should name the missing capability");
}

// Process capability gate: with process.exec granted and an allowlist, allowed binaries run while
// disallowed ones are refused. A spawnable contribution (formatter) declared without process.exec
// is rejected at registration. A data-scope plugin can write into ctx.workspace.data_dir().
void TestPluginHostProcessAndContributionCapabilities() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path data_home = temp_dir.path() / "data";
  const std::filesystem::path global_plugins = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  WriteFile(project_root / "README.md", "capability fixture\n");

  // process.exec with an allowlist of {"true"}: "true" runs, "false" is refused by the allowlist.
  WritePluginInit(
      global_plugins, "proc-grant",
      R"(local ide = require("microide")
return ide.plugin({
  id = "proc.grant",
  capabilities = { process = { exec = true, allow = { "true" } } },
  setup = function(ctx)
    ctx.commands.add("grant.allowed", function(ctx, args)
      ctx.log("allowed-exit:" .. tostring(ctx.process.run({"true"}).exit_code))
    end)
    ctx.commands.add("grant.denied", function(ctx, args)
      ctx.process.run({"false"})
    end)
  end,
})
)");

  // Formatter contribution without process.exec must be rejected at registration.
  WritePluginInit(
      global_plugins, "fmt-nogrant",
      R"(local ide = require("microide")
return ide.plugin({
  id = "fmt.nogrant",
  setup = function(ctx)
    ctx.formatters.add({
      id = "todo",
      language_id = "todo",
      label = "TODO",
      command = { "cat" },
    })
  end,
})
)");

  // Data-scope plugin writes into its sandboxed data dir via ctx.workspace.data_dir().
  WritePluginInit(
      global_plugins, "data-writer",
      R"(local ide = require("microide")
return ide.plugin({
  id = "data.writer",
  capabilities = { fs = { read = "data", write = "data" } },
  setup = function(ctx)
    ctx.commands.add("data.roundtrip", function(ctx, args)
      local dir = ctx.workspace.data_dir()
      ctx.log("data-dir:" .. tostring(dir ~= nil))
      local wrote = ctx.files.write_text(dir .. "/scratch.txt", "scratch")
      ctx.log("data-write:" .. tostring(wrote))
      ctx.log("data-read:" .. tostring(ctx.files.read_text(dir .. "/scratch.txt")))
    end)
  end,
})
)");

  ScopedPluginConfigHomeEnv config_env(config_home);
  ScopedEnvVar data_env("XDG_DATA_HOME", data_home.string());
  PluginHost host;
  host.SetCallbacks(MakePluginHostCallbacks());
  // Reload returns false here precisely because the fmt.nogrant plugin's setup is rejected; the
  // other (valid) plugins still load, which the assertions below confirm.
  Expect(!host.Reload(project_root),
         "reload should report the rejected-formatter error while loading the valid plugins");

  const auto has_message = [&](std::string_view needle) {
    for (const std::string& message : host.Messages()) {
      if (message.find(needle) != std::string::npos) {
        return true;
      }
    }
    return false;
  };
  const auto has_error = [&](std::string_view needle) {
    for (const std::string& message : host.Errors()) {
      if (message.find(needle) != std::string::npos) {
        return true;
      }
    }
    return false;
  };

  // Deferred contribution gate: the formatter declared without process.exec is rejected.
  Expect(host.ContributedFormatters().empty(),
         "a formatter declared without process.exec must not register");
  Expect(has_error("process.exec"), "the rejected formatter should report the missing capability");

  std::string error;
  Expect(host.ExecuteCommand("grant.allowed", {}, &error),
         "an allowlisted binary should run under the process capability");
  Expect(has_message("allowed-exit:0"), "the allowlisted binary should exit successfully");

  error.clear();
  Expect(!host.ExecuteCommand("grant.denied", {}, &error),
         "a binary outside the allowlist must be refused");
  Expect(error.find("allowlist") != std::string::npos,
         "the allowlist denial should name the allowlist");

  error.clear();
  Expect(host.ExecuteCommand("data.roundtrip", {}, &error), "data roundtrip command should execute");
  Expect(has_message("data-dir:true"), "data_dir() should return a path for a data-scope plugin");
  Expect(has_message("data-write:true"), "writing into the data dir should be permitted");
  Expect(has_message("data-read:scratch"), "reading back from the data dir should return the content");
}

// A contributed language server should carry a resolved kernel-confinement descriptor: enabled,
// with the project root among its write roots, and network blocked unless the plugin declared it.
// This locks the registration-time wiring that threads the sandbox down to AsyncSubprocess::Start.
void TestPluginHostLanguageServerSandboxResolved() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path global_plugins = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  WriteFile(project_root / "README.md", "lsp sandbox fixture\n");

  WritePluginInit(
      global_plugins, "lsp-sandbox",
      R"(local ide = require("microide")
return ide.plugin({
  id = "lsp.sandbox",
  capabilities = { process = { exec = true } },
  setup = function(ctx)
    ctx.lsp.add({ id = "markdown", language_id = "markdown", command = { "true" } })
  end,
})
)");

  ScopedPluginConfigHomeEnv config_env(config_home);
  PluginHost host;
  host.SetCallbacks(MakePluginHostCallbacks());
  Expect(host.Reload(project_root), "lsp sandbox plugin should load");

  Expect(host.ContributedLanguageServers().size() == 1,
         "the language server should register with process.exec granted");
  const platform::SubprocessSandbox& sandbox = host.ContributedLanguageServers().front().sandbox;
  Expect(sandbox.enabled, "the contributed language server should carry an enabled sandbox");
  Expect(!sandbox.allow_network,
         "network should be blocked for a server whose plugin did not declare it");
  const auto contains_project_root = [&](const std::vector<std::filesystem::path>& roots) {
    for (const std::filesystem::path& root : roots) {
      if (root == project_root) {
        return true;
      }
    }
    return false;
  };
  Expect(contains_project_root(sandbox.write_roots),
         "the project root should be writable inside the server sandbox");
  Expect(contains_project_root(sandbox.read_roots),
         "the project root should be readable inside the server sandbox");
}

// Wires a real PluginThread so plugin Lua runs off the calling thread, then proves
// the routing: a reactive event is dispatched to the worker (not run inline), its
// host mutations (status.update + log) are deferred to the mailbox rather than
// applied on the worker, and a blocking round-trip preserves the synchronous API.
// Run under TSAN this is the load-bearing check that the worker never touches a
// registry the calling thread reads — the adversarial render-path read below races
// the in-flight event on purpose.
void TestPluginHostRoutesEventsThroughWorkerThread() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path global_plugins = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path source = project_root / "src" / "main.js";
  WriteFile(project_root / "README.md", "worker routing fixture\n");
  WriteFile(source, "console.log('hello');\n");

  WritePluginInit(
      global_plugins, "worker-routing",
      R"(local ide = require("microide")
return ide.plugin({
  id = "worker.routing",
  setup = function(ctx)
    ctx.status.add({ id = "beat", text = "idle" })
    ctx.commands.add("worker.noop", function() end)
  end,
  on_cursor_move = function(ctx, buffer, pos)
    ctx.status.update("beat", { text = "moved:" .. tostring(pos.line) })
    ctx.log("cursor:" .. tostring(pos.line))
  end,
})
)");

  ScopedPluginConfigHomeEnv config_env(config_home);

  PluginHost host;
  host.SetCallbacks(MakePluginHostCallbacks());

  plugin::PluginThread thread;
  thread.SetWakeEventType(0);  // No SDL loop in this test; we drain the mailbox by hand.
  host.SetWorker(&thread);

  Expect(host.Reload(project_root), "worker routing fixture should load");
  Expect(thread.started(), "loading a plugin should lazily spawn the worker");

  bool has_idle = false;
  for (const auto& item : host.ContributedStatusItems()) {
    has_idle = has_idle || item.text == "idle";
  }
  Expect(has_idle, "setup should register the status item via the round-trip");

  // Fire-and-forget: this must post to the worker and return without running Lua
  // on this thread. The worker may now be executing on_cursor_move concurrently.
  host.OnCursorMove(source, 7, 3);

  // Adversarial race: read the render-path status view + revision while the event
  // is in flight. Safe only because the event defers its status mutation instead of
  // touching the registry on the worker. TSAN fails here if that contract breaks.
  (void)host.ContributedStatusItems();
  (void)host.StatusItemsRevision();

  // A blocking round-trip runs strictly after the queued event (FIFO on one worker),
  // so once it returns the event has completed and its deferred actions are queued.
  std::string error_message;
  std::string feedback;
  host.ExecuteCommand("worker.noop", {}, &error_message, &feedback);

  const int drained = thread.DrainMainThreadActions();
  Expect(drained > 0, "the event's deferred status update + log should reach the mailbox");

  bool moved = false;
  for (const auto& item : host.ContributedStatusItems()) {
    moved = moved || item.text == "moved:7";
  }
  Expect(moved, "the worker's status.update should apply on the UI-thread drain");
  Expect(std::any_of(host.Messages().begin(), host.Messages().end(),
                     [](const std::string& entry) { return entry == "worker.routing: cursor:7"; }),
         "the worker's ctx.log should be delivered through the mailbox drain");
  Expect(host.Errors().empty(), "routing an event through the worker should not surface errors");

  // Mirror the production teardown order: join the worker, drop the host's pointer,
  // then tear the host down inline.
  thread.Shutdown();
  host.SetWorker(nullptr);
  host.Shutdown();
}

// QueryCompletionsAsync must dispatch to the worker and deliver its result on the
// UI-thread mailbox drain — never synchronously and never on the worker. Verified
// under TSAN: the produce step runs on the worker, the deliver step on this thread.
void TestPluginHostQueryCompletionsAsyncDeliversThroughWorker() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path global_plugins = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path source = project_root / "src" / "main.js";
  WriteFile(project_root / "README.md", "async completion fixture\n");
  WriteFile(source, "x\n");

  WritePluginInit(
      global_plugins, "async-complete",
      R"(local ide = require("microide")
return ide.plugin({
  id = "async.complete",
  setup = function(ctx)
    ctx.commands.add("async.complete.noop", function() end)
    ctx.completion.add({
      id = "words", language_id = "javascript", trigger_characters = { "." },
      provide = function(buffer, position, trigger)
        return { { label = "hello", insert_text = "hello" } }
      end,
    })
  end,
})
)");

  ScopedPluginConfigHomeEnv config_env(config_home);

  PluginHost host;
  host.SetCallbacks(MakePluginHostCallbacks());
  plugin::PluginThread thread;
  thread.SetWakeEventType(0);
  host.SetWorker(&thread);

  Expect(host.Reload(project_root), "async completion fixture should load");

  std::vector<PluginHost::CompletionCandidate> delivered_items;
  bool delivered = false;
  host.QueryCompletionsAsync(
      "javascript", source, 1, 1, "",
      [&](std::vector<PluginHost::CompletionCandidate> items, std::string /*error*/) {
        delivered_items = std::move(items);
        delivered = true;
      });
  Expect(!delivered,
         "with a worker wired the completion result must not be delivered synchronously");

  // A blocking round-trip runs after the queued async query (FIFO), so the query
  // has completed once it returns; its result now sits in the mailbox, undelivered
  // until the drain.
  std::string error_message;
  std::string feedback;
  host.ExecuteCommand("async.complete.noop", {}, &error_message, &feedback);
  Expect(!delivered, "the result is delivered on the drain, not during the round-trip");

  const int drained = thread.DrainMainThreadActions();
  Expect(drained > 0, "the completion result should reach the mailbox");
  Expect(delivered, "draining should deliver the async completion result");
  Expect(delivered_items.size() == 1 && delivered_items.front().label == "hello",
         "the async completion query should return the provider's item");

  thread.Shutdown();
  host.SetWorker(nullptr);
  host.Shutdown();
}

// QueryHoverAsync dispatches to the worker and delivers on the UI-thread drain.
// Verified under TSAN.
void TestPluginHostQueryHoverAsyncDeliversThroughWorker() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path global_plugins = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path source = project_root / "src" / "main.js";
  WriteFile(project_root / "README.md", "async hover fixture\n");
  WriteFile(source, "x\n");

  WritePluginInit(
      global_plugins, "async-hover",
      R"(local ide = require("microide")
return ide.plugin({
  id = "async.hover",
  setup = function(ctx)
    ctx.commands.add("async.hover.noop", function() end)
    ctx.hover.add({
      id = "h",
      provide = function(buffer, position)
        return { title = "T", content = "hover@" .. tostring(position.line) }
      end,
    })
  end,
})
)");

  ScopedPluginConfigHomeEnv config_env(config_home);

  PluginHost host;
  host.SetCallbacks(MakePluginHostCallbacks());
  plugin::PluginThread thread;
  thread.SetWakeEventType(0);
  host.SetWorker(&thread);

  Expect(host.Reload(project_root), "async hover fixture should load");

  bool delivered = false;
  bool ok = false;
  PluginHost::HoverResult hover;
  host.QueryHoverAsync(source, 1, 1,
                       [&](bool query_ok, PluginHost::HoverResult result) {
                         ok = query_ok;
                         hover = std::move(result);
                         delivered = true;
                       });
  Expect(!delivered, "the hover result must not be delivered synchronously with a worker wired");

  std::string error_message;
  std::string feedback;
  host.ExecuteCommand("async.hover.noop", {}, &error_message, &feedback);
  const int drained = thread.DrainMainThreadActions();
  Expect(drained > 0 && delivered && ok, "draining should deliver the async hover result");
  Expect(hover.content == "hover@1", "the async hover query should return the provider's content");

  thread.Shutdown();
  host.SetWorker(nullptr);
  host.Shutdown();
}

// ExecuteCommandAsync resolves "handled" synchronously (HasCommand) but runs the
// handler on the worker and delivers its outcome on the UI-thread drain. Verified
// under TSAN.
void TestPluginHostExecuteCommandAsyncDeliversThroughWorker() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path global_plugins = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  WriteFile(project_root / "README.md", "async command fixture\n");

  WritePluginInit(
      global_plugins, "async-command",
      R"(local ide = require("microide")
return ide.plugin({
  id = "async.cmd",
  setup = function(ctx)
    ctx.commands.add("async.cmd.run", function(ctx2, args)
      return "did:" .. tostring(args[1] or "none")
    end)
    ctx.commands.add("async.cmd.noop", function() end)
  end,
})
)");

  ScopedPluginConfigHomeEnv config_env(config_home);

  PluginHost host;
  host.SetCallbacks(MakePluginHostCallbacks());
  plugin::PluginThread thread;
  thread.SetWakeEventType(0);
  host.SetWorker(&thread);

  Expect(host.Reload(project_root), "async command fixture should load");
  Expect(host.HasCommand("async.cmd.run"), "a registered command should resolve synchronously");
  Expect(!host.HasCommand("async.cmd.missing"), "an unknown command should not resolve");

  bool delivered = false;
  bool ran = false;
  std::string feedback;
  host.ExecuteCommandAsync("async.cmd.run", {"x"},
                           [&](bool command_ran, std::string /*error*/, std::string command_feedback) {
                             ran = command_ran;
                             feedback = std::move(command_feedback);
                             delivered = true;
                           });
  Expect(!delivered, "the command outcome must not be delivered synchronously with a worker wired");

  // Quiesce the worker with a blocking round-trip (FIFO), then drain.
  std::string error_message;
  std::string round_trip_feedback;
  host.ExecuteCommand("async.cmd.noop", {}, &error_message, &round_trip_feedback);
  const int drained = thread.DrainMainThreadActions();
  Expect(drained > 0 && delivered && ran, "draining should deliver the async command outcome");
  Expect(feedback == "did:x", "the command's returned feedback should round-trip to the callback");

  thread.Shutdown();
  host.SetWorker(nullptr);
  host.Shutdown();
}

// ReloadAsync must run the plugin load on the worker WITHOUT blocking the UI thread,
// then publish the rebuilt contribution snapshot and fire on_complete on the mailbox
// drain. Until the drain, the UI keeps reading the previous (empty) published view.
// Under TSAN this is the load-bearing check that a detached reload mutates the live
// registries on the worker while the calling thread reads only the published snapshot.
void TestPluginHostReloadAsyncPublishesOnDrain() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path global_plugins = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  WriteFile(project_root / "README.md", "async reload fixture\n");

  WritePluginInit(
      global_plugins, "async-reload",
      R"(local ide = require("microide")
return ide.plugin({
  id = "async.reload",
  setup = function(ctx)
    ctx.commands.add("async.reload.cmd", function() end)
    ctx.commands.add("async.reload.noop", function() end)
    ctx.status.add({ id = "badge", text = "ready" })
  end,
})
)");

  ScopedPluginConfigHomeEnv config_env(config_home);

  PluginHost host;
  host.SetCallbacks(MakePluginHostCallbacks());
  plugin::PluginThread thread;
  thread.SetWakeEventType(0);  // No SDL loop; drain the mailbox by hand.
  host.SetWorker(&thread);

  bool completed = false;
  bool clean = false;
  host.ReloadAsync(project_root, [&](bool ok) {
    completed = true;
    clean = ok;
  });
  Expect(!completed, "ReloadAsync must not complete synchronously with a worker wired");
  Expect(!host.HasCommand("async.reload.cmd"),
         "a reloaded command must not be visible before the snapshot publishes");
  Expect(host.ContributedStatusItems().empty(),
         "status items must not be visible before the snapshot publishes");
  Expect(thread.started(), "loading a plugin should lazily spawn the worker");

  // A blocking round-trip runs strictly after the queued reload (FIFO on one worker),
  // so once it returns the reload has finished and posted its completion to the mailbox.
  std::string error_message;
  std::string feedback;
  host.ExecuteCommand("async.reload.noop", {}, &error_message, &feedback);
  const int drained = thread.DrainMainThreadActions();
  Expect(drained > 0 && completed && clean,
         "draining should publish the snapshot and fire on_complete cleanly");
  Expect(host.HasCommand("async.reload.cmd"),
         "the reloaded command should resolve after the snapshot publishes");
  bool has_badge = false;
  for (const auto& item : host.ContributedStatusItems()) {
    has_badge = has_badge || item.text == "ready";
  }
  Expect(has_badge, "the reloaded status item should appear in the published view");

  thread.Shutdown();
  host.SetWorker(nullptr);
  host.Shutdown();
}

}  // namespace

void RegisterPluginHostTests(std::vector<TestCase>& tests) {
  AddTest(tests, "PluginHost/FilesystemSandbox", TestPluginHostFilesystemSandbox);
  AddTest(tests, "PluginHost/LanguageServerSandboxResolved",
          TestPluginHostLanguageServerSandboxResolved);
  AddTest(tests, "PluginHost/ProcessAndContributionCapabilities",
          TestPluginHostProcessAndContributionCapabilities);
  AddTest(tests, "PluginHost/LoadsPluginsAndDispatchesLifecycle",
          TestPluginHostLoadsPluginsAndDispatchesLifecycle);
  AddTest(tests, "PluginHost/IgnoresProjectLocalPlugins",
          TestPluginHostIgnoresProjectLocalPlugins);
  AddTest(tests, "PluginHost/LoadsUserConfigPlugins", TestPluginHostLoadsUserConfigPlugins);
  AddTest(tests, "PluginHost/PrefersUserPluginOverProjectLocalPlugin",
          TestPluginHostPrefersUserPluginOverProjectLocalPlugin);
  AddTest(tests, "PluginHost/ReloadIgnoresLateProjectLocalPlugin",
          TestPluginHostReloadIgnoresLateProjectLocalPlugin);
  AddTest(tests, "PluginHost/AssetMonitorWatchesUserPluginRootOnly",
          TestPluginAssetMonitorWatchesUserPluginRootOnly);
  AddTest(tests, "PluginHost/RejectsDuplicatePluginIds",
          TestPluginHostRejectsDuplicatePluginIds);
  AddTest(tests, "PluginHost/SkipsBackupPluginDirectories",
          TestPluginHostSkipsBackupPluginDirectories);
  AddTest(tests, "PluginHost/LuaRuntimeMatchesDocumentedStdlib",
          TestPluginHostLuaRuntimeMatchesDocumentedStdlib);
  AddTest(tests, "PluginHost/PluginsUseIsolatedLuaStates",
          TestPluginHostPluginsUseIsolatedLuaStates);
  AddTest(tests, "PluginHost/WatchdogAbortsRunawayCall",
          TestPluginHostWatchdogAbortsRunawayCall);
  AddTest(tests, "PluginHost/Phase2Apis", TestPluginHostPhase2Apis);
  AddTest(tests, "PluginHost/Phase2StatusApis", TestPluginHostPhase2StatusApis);
  AddTest(tests, "PluginHost/Phase3DiagnosticsApis", TestPluginHostPhase3DiagnosticsApis);
  AddTest(tests, "PluginHost/Phase3HoverApis", TestPluginHostPhase3HoverApis);
  AddTest(tests, "PluginHost/Phase3RuntimeApis", TestPluginHostPhase3RuntimeApis);
  AddTest(tests, "PluginHost/RunAsyncInvokesCallbackSynchronously",
          TestPluginHostRunAsyncInvokesCallbackSynchronously);
  AddTest(tests, "PluginHost/RoutesEventsThroughWorkerThread",
          TestPluginHostRoutesEventsThroughWorkerThread);
  AddTest(tests, "PluginHost/QueryCompletionsAsyncDeliversThroughWorker",
          TestPluginHostQueryCompletionsAsyncDeliversThroughWorker);
  AddTest(tests, "PluginHost/QueryHoverAsyncDeliversThroughWorker",
          TestPluginHostQueryHoverAsyncDeliversThroughWorker);
  AddTest(tests, "PluginHost/ExecuteCommandAsyncDeliversThroughWorker",
          TestPluginHostExecuteCommandAsyncDeliversThroughWorker);
  AddTest(tests, "PluginHost/ReloadAsyncPublishesOnDrain",
          TestPluginHostReloadAsyncPublishesOnDrain);
  AddTest(tests, "PluginHost/Phase4ContributionApis", TestPluginHostPhase4ContributionApis);
  AddTest(tests, "PluginHost/Phase5WorkspaceApis", TestPluginHostPhase5WorkspaceApis);
  AddTest(tests, "PluginHost/Phase5LspApis", TestPluginHostPhase5LspApis);
  AddTest(tests, "PluginHost/DebugAdapterRegistration", TestPluginHostDebugAdapterRegistration);
  AddTest(tests, "PluginHost/DebugAdapterRequiresProcessExec",
          TestPluginHostDebugAdapterRequiresProcessExec);
  AddTest(tests, "PluginHost/LaunchConfigRegistration", TestPluginHostLaunchConfigRegistration);
  AddTest(tests, "PluginHost/LaunchConfigRequiresProcessExec",
          TestPluginHostLaunchConfigRequiresProcessExec);
  AddTest(tests, "PluginHost/RepoTypescriptLspPluginUsesAbsoluteProjectBinary",
          TestRepoTypescriptLspPluginUsesAbsoluteProjectBinary);
  AddTest(tests, "PluginHost/RepoCppLspPluginRegistersClangdForCLikeLanguages",
          TestRepoCppLspPluginRegistersClangdForCLikeLanguages);
  AddTest(tests, "PluginHost/ProcessRunReportsArgumentErrorsWithoutCorruptingState",
          TestPluginHostProcessRunReportsArgumentErrorsWithoutCorruptingState);
  AddTest(tests, "PluginHost/BatchedCommandRegistrationSortsOnce",
          TestPluginHostBatchedCommandRegistrationSortsOnce);
  AddTest(tests, "PluginHost/CommandReturnsFeedback", TestPluginHostCommandReturnsFeedback);
  AddTest(tests, "PluginHost/NotifyInvokesCallback", TestPluginHostNotifyInvokesCallback);
  AddTest(tests, "PluginHost/DisabledPluginsSkipSetupButRemainListed",
          TestPluginHostDisabledPluginsSkipSetupButRemainListed);
}

}  // namespace microide::tests
