#include "TestSupport.h"

#include "plugin/PluginHost.h"
#include "plugin/PluginInstallRoot.h"
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
  setup = function(ctx)
    ctx.lsp.add({
      id = "markdown",
      language_id = "markdown",
      command = { "python3", "fake-lsp.py" }
    })
  end
})
)");

  ScopedPluginConfigHomeEnv config_env(config_home);

  PluginHost host;
  host.SetCallbacks(MakePluginHostCallbacks());

  Expect(host.Reload(project_root), "phase5 lsp plugin should reload successfully");
  Expect(host.ContributedLanguageServers().size() == 1,
         "ctx.lsp.add should register one language server");
  Expect(host.ContributedLanguageServers().front().id == "phase5-lsp.markdown" &&
             host.ContributedLanguageServers().front().language_id == "markdown" &&
             host.ContributedLanguageServers().front().command.size() == 2 &&
             host.ContributedLanguageServers().front().command.front() == "python3",
         "language server contributions should preserve ids, language ids, and commands");
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
  Expect(server.id == "typescript-lsp.typescript" && server.language_id == "typescript",
         "repo typescript-lsp plugin should preserve its ids");
  Expect(server.command.size() == 2 &&
             server.command.front() ==
                 (project_root / "node_modules" / ".bin" / "typescript-language-server").generic_string() &&
             server.command.back() == "--stdio",
         "repo typescript-lsp plugin should use an absolute project-local language server path");
}

void TestPluginHostCancelsAsyncCallbacksOnReload() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path global_plugins = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path source = project_root / "src" / "main.js";
  WriteFile(project_root / "README.md", "async reload fixture\n");
  WriteFile(source, "console.log('hello');\n");

  WritePluginInit(
      global_plugins, "async-reload",
      R"(local ide = require("microide")
return ide.plugin({
  id = "async.reload",
  on_buffer_open = function(ctx, buffer)
    ctx.process.run_async({"sh", "-lc", "sleep 0.5; printf done"}, nil, function(result)
      ctx.log("async-complete:" .. tostring(result.exit_code))
    end)
  end
})
)");

  ScopedPluginConfigHomeEnv config_env(config_home);

  PluginHost host;
  host.SetCallbacks(MakePluginHostCallbacks());

  Expect(host.Reload(project_root), "async reload fixture should load");
  host.OnBufferOpen(source);
  Expect(host.PendingAsyncProcessCount() > 0,
         "buffer open should leave an async process in flight before reload");
  const auto reload_start = std::chrono::steady_clock::now();
  Expect(host.Reload(project_root), "reloading while an async callback is pending should succeed");
  const auto reload_elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                            reload_start);
  Expect(reload_elapsed < std::chrono::milliseconds(200),
         "reloading while an async callback is pending should not wait for the subprocess");

  std::this_thread::sleep_for(std::chrono::milliseconds(700));
  host.ConsumeAsyncProcessCallbacks();
  Expect(host.PendingAsyncProcessCount() == 0,
         "reload should drain or discard async callbacks from the previous plugin state");
  Expect(std::none_of(host.Messages().begin(), host.Messages().end(), [](const std::string& entry) {
           return entry == "async.reload: async-complete:0";
         }),
         "reload should discard async callbacks captured by the old plugin state");
  Expect(host.Errors().empty(),
         "reloading while async callbacks are pending should not report callback errors");
}

void TestPluginHostCancelsAsyncCallbacksOnShutdown() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path global_plugins = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path source = project_root / "src" / "main.js";
  WriteFile(project_root / "README.md", "async shutdown fixture\n");
  WriteFile(source, "console.log('hello');\n");

  WritePluginInit(
      global_plugins, "async-shutdown",
      R"(local ide = require("microide")
return ide.plugin({
  id = "async.shutdown",
  on_buffer_open = function(ctx, buffer)
    ctx.process.run_async({"sh", "-lc", "sleep 0.5; printf done"}, nil, function(result)
      ctx.log("async-complete:" .. tostring(result.exit_code))
    end)
  end
})
)");

  ScopedPluginConfigHomeEnv config_env(config_home);

  PluginHost host;
  host.SetCallbacks(MakePluginHostCallbacks());

  Expect(host.Reload(project_root), "async shutdown fixture should load");
  host.OnBufferOpen(source);
  Expect(host.PendingAsyncProcessCount() > 0,
         "buffer open should leave an async process in flight before shutdown");
  const auto shutdown_start = std::chrono::steady_clock::now();
  host.Shutdown();
  const auto shutdown_elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                            shutdown_start);
  Expect(shutdown_elapsed < std::chrono::milliseconds(200),
         "shutting down with a pending async callback should not wait for the subprocess");
  Expect(host.PendingAsyncProcessCount() == 0,
         "shutdown should stop reporting cancelled async processes as pending work");

  std::this_thread::sleep_for(std::chrono::milliseconds(700));
  host.ConsumeAsyncProcessCallbacks();
  Expect(host.PendingAsyncProcessCount() == 0,
         "shutdown should drain or discard pending async callbacks");
  Expect(std::none_of(host.Messages().begin(), host.Messages().end(), [](const std::string& entry) {
           return entry == "async.shutdown: async-complete:0";
         }),
         "shutdown should discard async callbacks after Lua teardown");
  Expect(host.Errors().empty(),
         "shutting down with pending async callbacks should not report callback errors");
}

void TestPluginHostRapidReloadDrainsAsyncWorkers() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path global_plugins = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path source = project_root / "src" / "main.js";
  WriteFile(project_root / "README.md", "rapid reload fixture\n");
  WriteFile(source, "console.log('hello');\n");

  WritePluginInit(
      global_plugins, "rapid-reload",
      R"(local ide = require("microide")
return ide.plugin({
  id = "rapid.reload",
  on_buffer_open = function(ctx, buffer)
    ctx.process.run_async({"sh", "-lc", "sleep 0.3; printf done"}, nil, function(result)
      ctx.log("async-complete:" .. tostring(result.exit_code))
    end)
  end
})
)");

  ScopedPluginConfigHomeEnv config_env(config_home);

  PluginHost host;
  host.SetCallbacks(MakePluginHostCallbacks());

  Expect(host.Reload(project_root), "rapid reload fixture should load");

  // Drive the reload-while-inflight pattern that previously crashed: schedule an
  // async subprocess, reload before it finishes, repeat. The drain seam in
  // Reload must observe the cancellation and let the still-running detached
  // worker thread complete safely on its own. Under TSAN this exercises the
  // worker fetch_sub + cv notify path against the drain seam wait_for path.
  for (int i = 0; i < 8; ++i) {
    host.OnBufferOpen(source);
    Expect(host.PendingAsyncProcessCount() > 0,
           "buffer open should leave an async process in flight before reload");
    const auto reload_start = std::chrono::steady_clock::now();
    Expect(host.Reload(project_root),
           "rapid reload should succeed even with workers still in flight");
    const auto reload_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - reload_start);
    Expect(reload_elapsed < std::chrono::milliseconds(250),
           "rapid reload should bound on the drain deadline, not on the subprocess");
  }

  // Final shutdown after the rapid sequence must also drain cleanly.
  host.OnBufferOpen(source);
  const auto shutdown_start = std::chrono::steady_clock::now();
  host.Shutdown();
  const auto shutdown_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - shutdown_start);
  Expect(shutdown_elapsed < std::chrono::milliseconds(250),
         "shutdown after rapid reload should bound on the drain deadline");

  // Wait for any still-detached subprocess workers to retire, then confirm no
  // callback fired after Shutdown returned.
  std::this_thread::sleep_for(std::chrono::milliseconds(600));
  host.ConsumeAsyncProcessCallbacks();
  Expect(host.PendingAsyncProcessCount() == 0,
         "shutdown should leave no async work pending after workers retire");
  Expect(std::none_of(host.Messages().begin(), host.Messages().end(),
                      [](const std::string& entry) {
                        return entry == "rapid.reload: async-complete:0";
                      }),
         "no callback from a torn-down plugin should fire after shutdown");
  Expect(host.Errors().empty(),
         "rapid reload-and-shutdown sequence should not surface callback errors");
}

}  // namespace

void RegisterPluginHostTests(std::vector<TestCase>& tests) {
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
  AddTest(tests, "PluginHost/Phase2Apis", TestPluginHostPhase2Apis);
  AddTest(tests, "PluginHost/Phase2StatusApis", TestPluginHostPhase2StatusApis);
  AddTest(tests, "PluginHost/Phase3DiagnosticsApis", TestPluginHostPhase3DiagnosticsApis);
  AddTest(tests, "PluginHost/Phase3HoverApis", TestPluginHostPhase3HoverApis);
  AddTest(tests, "PluginHost/Phase3RuntimeApis", TestPluginHostPhase3RuntimeApis);
  AddTest(tests, "PluginHost/CancelsAsyncCallbacksOnReload",
          TestPluginHostCancelsAsyncCallbacksOnReload);
  AddTest(tests, "PluginHost/CancelsAsyncCallbacksOnShutdown",
          TestPluginHostCancelsAsyncCallbacksOnShutdown);
  AddTest(tests, "PluginHost/RapidReloadDrainsAsyncWorkers",
          TestPluginHostRapidReloadDrainsAsyncWorkers);
  AddTest(tests, "PluginHost/Phase4ContributionApis", TestPluginHostPhase4ContributionApis);
  AddTest(tests, "PluginHost/Phase5WorkspaceApis", TestPluginHostPhase5WorkspaceApis);
  AddTest(tests, "PluginHost/Phase5LspApis", TestPluginHostPhase5LspApis);
  AddTest(tests, "PluginHost/RepoTypescriptLspPluginUsesAbsoluteProjectBinary",
          TestRepoTypescriptLspPluginUsesAbsoluteProjectBinary);
}

}  // namespace microide::tests
