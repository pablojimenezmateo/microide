#include "TestSupport.h"

#include "plugin/PluginHost.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace microide::tests {
namespace {

using microide::plugin::PluginHost;

void WritePluginInit(const std::filesystem::path& root,
                     std::string_view directory_name,
                     std::string_view content) {
  WriteFile(root / directory_name / "init.lua", std::string(content));
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

void TestPluginHostLoadsPluginsAndDispatchesLifecycle() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path global_plugins = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path project_plugins = project_root / ".microide" / "plugins";

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
      project_plugins, "project-sample",
      R"(local ide = require("microide")
return ide.plugin({
  id = "project.sample",
  setup = function(ctx)
    ctx.log("setup:project")
    ctx.commands.add("project.open-readme", function(ctx, args)
      ctx.workspace.open_file("README.md")
    end)
  end,
  on_project_open = function(ctx, project)
    ctx.log("project-open:project:" .. project.name)
  end,
  on_project_close = function(ctx, project)
    ctx.log("project-close:project:" .. project.name)
  end,
  on_buffer_open = function(ctx, buffer)
    ctx.log("buffer-open:project:" .. buffer.relative_path)
  end,
  on_buffer_save = function(ctx, buffer)
    ctx.log("buffer-save:project:" .. buffer.relative_path)
  end,
  shutdown = function(ctx)
    ctx.log("shutdown:project")
  end
})
)");

  ScopedEnvVar xdg_config_home("XDG_CONFIG_HOME", config_home.string());

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
  Expect(host.LoadedPluginCount() == 2, "plugin host should load global and project plugins");
  Expect(host.CommandNames().size() == 2, "plugin host should expose both registered commands");
  Expect(std::find(host.CommandNames().begin(), host.CommandNames().end(), "global.echo") !=
             host.CommandNames().end(),
         "plugin host should expose global plugin commands");
  Expect(std::find(host.CommandNames().begin(), host.CommandNames().end(), "project.open-readme") !=
             host.CommandNames().end(),
         "plugin host should expose project-local plugin commands");

  const std::vector<std::string>& load_messages = host.Messages();
  Expect(load_messages.size() >= 4, "plugin load should record setup and project-open messages");
  Expect(load_messages[0] == "global.sample: setup:global",
         "global plugins should set up before project-local plugins");
  Expect(load_messages[1] == "project.sample: setup:project",
         "project plugin setup should follow global setup");
  Expect(load_messages[2] == "global.sample: project-open:global:project",
         "global project-open hook should run after setup");
  Expect(load_messages[3] == "project.sample: project-open:project:project",
         "project-local project-open hook should run after global hooks");

  host.ClearMessages();
  host.OnBufferOpen(project_root / "src" / "main.cpp");
  host.OnBufferSave(project_root / "src" / "main.cpp");
  Expect(host.Messages().size() == 4,
         "buffer hooks should run for both loaded plugins");
  Expect(host.Messages()[0] == "global.sample: buffer-open:global:src/main.cpp",
         "global buffer-open hook should receive project-relative paths");
  Expect(host.Messages()[3] == "project.sample: buffer-save:project:src/main.cpp",
         "project-local buffer-save hook should receive project-relative paths");

  host.ClearMessages();
  std::string command_error;
  Expect(host.ExecuteCommand("global.echo", {"alpha", "beta"}, &command_error),
         "plugin command dispatch should succeed");
  Expect(command_error.empty(), "successful plugin commands should clear error output");
  Expect(!host.Messages().empty() &&
             host.Messages().back() == "global.sample: command:global:alpha,beta",
         "plugin commands should receive argv-style arguments");

  Expect(host.ExecuteCommand("project.open-readme", {}, &command_error),
         "workspace.open_file should be callable from plugin commands");
  Expect(!opened_paths.empty() && opened_paths.back() == (project_root / "README.md").lexically_normal(),
         "workspace.open_file should resolve relative paths against the active project");

  host.ClearMessages();
  host.Shutdown();
  Expect(host.Messages().size() == 4,
         "shutdown should emit project-close and shutdown hooks for each plugin");
  Expect(host.Messages()[0] == "global.sample: project-close:global:project",
         "shutdown should close the active project before tearing plugins down");
  Expect(host.Messages()[3] == "project.sample: shutdown:project",
         "shutdown should run each plugin's shutdown hook");
}

void TestPluginHostRejectsDuplicatePluginIds() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path global_plugins = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path project_plugins = project_root / ".microide" / "plugins";

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
      project_plugins, "dup-b",
      R"(local ide = require("microide")
return ide.plugin({
  id = "dup",
  setup = function(ctx)
    ctx.commands.add("dup.project", function(ctx, args) end)
  end
})
)");

  ScopedEnvVar xdg_config_home("XDG_CONFIG_HOME", config_home.string());

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

void TestPluginHostPhase2Apis() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path global_plugins = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  WriteFile(project_root / "README.md", "plugin host phase2\n");

  WritePluginInit(
      global_plugins, "phase2",
      R"(local ide = require("microide")
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
)");

  ScopedEnvVar xdg_config_home("XDG_CONFIG_HOME", config_home.string());
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
  Expect(std::find(host.Messages().begin(), host.Messages().end(),
                   "phase2: cat:0:stdin payload\n") != host.Messages().end(),
         "ctx.process.run should capture stdout and stdin");
  const auto pwd_message =
      std::find_if(host.Messages().begin(), host.Messages().end(), [](const std::string& message) {
        return message.rfind("phase2: pwd:0:", 0) == 0;
      });
  Expect(pwd_message != host.Messages().end() &&
             pwd_message->find(project_root.lexically_normal().string()) != std::string::npos,
         "ctx.process.run should honor cwd relative to the active project");
  Expect(std::find(host.Messages().begin(), host.Messages().end(),
                   "phase2: envset:0:plugin-value") != host.Messages().end(),
         "ctx.process.run should apply environment overrides");
  Expect(std::find(host.Messages().begin(), host.Messages().end(),
                   "phase2: envunset:0:unset") != host.Messages().end(),
         "ctx.process.run should allow clearing inherited environment variables");

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
                                          (project_root / "README.md").lexically_normal().string() +
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

  ScopedEnvVar xdg_config_home("XDG_CONFIG_HOME", config_home.string());

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

  ScopedEnvVar xdg_config_home("XDG_CONFIG_HOME", config_home.string());

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

  ScopedEnvVar xdg_config_home("XDG_CONFIG_HOME", config_home.string());

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

  ScopedEnvVar xdg_config_home("XDG_CONFIG_HOME", config_home.string());

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

  ScopedEnvVar xdg_config_home("XDG_CONFIG_HOME", config_home.string());

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

  ScopedEnvVar xdg_config_home("XDG_CONFIG_HOME", config_home.string());

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
         "phase5 workspace command should execute");
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

  ScopedEnvVar xdg_config_home("XDG_CONFIG_HOME", config_home.string());

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

}  // namespace

void RegisterPluginHostTests(std::vector<TestCase>& tests) {
  AddTest(tests, "PluginHost/LoadsPluginsAndDispatchesLifecycle",
          TestPluginHostLoadsPluginsAndDispatchesLifecycle);
  AddTest(tests, "PluginHost/RejectsDuplicatePluginIds",
          TestPluginHostRejectsDuplicatePluginIds);
  AddTest(tests, "PluginHost/Phase2Apis", TestPluginHostPhase2Apis);
  AddTest(tests, "PluginHost/Phase2StatusApis", TestPluginHostPhase2StatusApis);
  AddTest(tests, "PluginHost/Phase3DiagnosticsApis", TestPluginHostPhase3DiagnosticsApis);
  AddTest(tests, "PluginHost/Phase3HoverApis", TestPluginHostPhase3HoverApis);
  AddTest(tests, "PluginHost/Phase3RuntimeApis", TestPluginHostPhase3RuntimeApis);
  AddTest(tests, "PluginHost/Phase4ContributionApis", TestPluginHostPhase4ContributionApis);
  AddTest(tests, "PluginHost/Phase5WorkspaceApis", TestPluginHostPhase5WorkspaceApis);
  AddTest(tests, "PluginHost/Phase5LspApis", TestPluginHostPhase5LspApis);
}

}  // namespace microide::tests
