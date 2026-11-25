#include "TestSupport.h"
#include "TestSupportSdl.h"
#include "support/SoftwareCanvas.h"

#include "util/JsonValue.h"
#include "util/PerformanceCounters.h"
#include "workspace/control/ControlProtocol.h"
#include "workspace/registries/WorkspaceSettingsRegistry.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace microide::tests {
namespace {

using microide::workspace::FindBuiltinSettingSpec;
using microide::workspace::SettingSpec;
using microide::workspace::SettingType;
using microide::workspace::WorkspaceShell;
using WorkspaceShellTestAccess = microide::workspace::WorkspaceShell::TestAccess;

bool RootsContain(const std::vector<std::filesystem::path>& roots,
                  const std::filesystem::path& target) {
  const std::filesystem::path normalized = target.lexically_normal();
  return std::any_of(roots.begin(), roots.end(), [&](const std::filesystem::path& root) {
    return root.lexically_normal() == normalized;
  });
}

void TestSetSettingCommandFlipsAndRejects() {
  TemporaryDirectory temp;
  ScopedAppHomes homes(temp.path() / "state", temp.path() / "config");

  WorkspaceShell shell;
  shell.Initialize({});

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "set-setting control.enabled true"),
         "set-setting should succeed for a known setting");
  Expect(WorkspaceShellTestAccess::GetSettingValue(shell, "control.enabled") == "true",
         "set-setting should flip the value through the chokepoint");

  Expect(!WorkspaceShellTestAccess::ExecuteCommandLine(shell, "set-setting not.a.real.setting x"),
         "set-setting should reject an unknown id");
  Expect(!WorkspaceShellTestAccess::ExecuteCommandLine(shell, "set-setting"),
         "set-setting with no arguments should reject");
}

void TestSetSettingStoresClampedBuiltinValue() {
  TemporaryDirectory temp;
  ScopedAppHomes homes(temp.path() / "state", temp.path() / "config");

  WorkspaceShell shell;
  shell.Initialize({});

  const SettingSpec* spec = FindBuiltinSettingSpec("editor.font_size");
  Expect(spec != nullptr && spec->type == SettingType::Int, "font_size must be an int spec");

  Expect(WorkspaceShellTestAccess::SetSettingValue(shell, "editor.font_size", "999"),
         "an out-of-range int write should still succeed (clamped)");
  Expect(WorkspaceShellTestAccess::GetSettingValue(shell, "editor.font_size") ==
             std::to_string(spec->max_int),
         "the stored value must be clamped to the spec max, not the raw 999");

  Expect(WorkspaceShellTestAccess::SetSettingValue(shell, "editor.font_size", "1"),
         "a below-range int write should succeed (clamped)");
  Expect(WorkspaceShellTestAccess::GetSettingValue(shell, "editor.font_size") ==
             std::to_string(spec->min_int),
         "the stored value must be clamped to the spec min, not the raw 1");
}

void TestSettingsStepperStopsAtTheBounds() {
  TemporaryDirectory temp;
  ScopedAppHomes homes(temp.path() / "state", temp.path() / "config");

  WorkspaceShell shell;
  shell.Initialize({});

  const SettingSpec* tab = FindBuiltinSettingSpec("editor.tab_size");
  Expect(tab != nullptr && tab->type == SettingType::Int, "tab_size must be an int spec");
  WorkspaceShellTestAccess::SetSettingValue(shell, "editor.tab_size", std::to_string(tab->max_int));
  WorkspaceShellTestAccess::StepSetting(shell, "editor.tab_size", true);
  Expect(WorkspaceShellTestAccess::GetSettingValue(shell, "editor.tab_size") == std::to_string(tab->max_int),
         "stepping up from the maximum stays at the maximum");
  WorkspaceShellTestAccess::SetSettingValue(shell, "editor.tab_size", std::to_string(tab->min_int));
  WorkspaceShellTestAccess::StepSetting(shell, "editor.tab_size", false);
  Expect(WorkspaceShellTestAccess::GetSettingValue(shell, "editor.tab_size") == std::to_string(tab->min_int),
         "stepping down from the minimum stays at the minimum");

  const SettingSpec* scale = FindBuiltinSettingSpec("ui.scale");
  Expect(scale != nullptr && scale->type == SettingType::Float, "ui.scale must be a float spec");
  WorkspaceShellTestAccess::SetSettingValue(shell, "ui.scale", microide::workspace::SerializeSettingValue(scale->max_float));
  WorkspaceShellTestAccess::StepSetting(shell, "ui.scale", true);
  Expect(WorkspaceShellTestAccess::GetSettingValue(shell, "ui.scale") == microide::workspace::SerializeSettingValue(scale->max_float),
         "stepping the UI scale up from its maximum stays there");

  const SettingSpec* autosave = FindBuiltinSettingSpec("editor.autosave");
  Expect(autosave != nullptr && autosave->type == SettingType::Enum, "autosave must be an enum spec");
  WorkspaceShellTestAccess::SetSettingValue(shell, "editor.autosave", std::string(autosave->enum_values.back().value));
  WorkspaceShellTestAccess::StepSetting(shell, "editor.autosave", true);
  Expect(WorkspaceShellTestAccess::GetSettingValue(shell, "editor.autosave") == std::string(autosave->enum_values.front().value),
         "an enum keeps cycling past its last option");
}

void TestTransientSettingNotPersisted() {
  TemporaryDirectory temp;
  ScopedAppHomes homes(temp.path() / "state", temp.path() / "config");

  WorkspaceShell shell;
  shell.Initialize({});

  Expect(WorkspaceShellTestAccess::SetSettingValue(shell, "control.enabled", "true"),
         "durable set should succeed");
  Expect(WorkspaceShellTestAccess::SetSettingValueTransient(shell, "debug.enabled", "true"),
         "transient set should succeed");

  Expect(WorkspaceShellTestAccess::IsSettingTransient(shell, "debug.enabled"),
         "transiently-set key should be marked transient");
  Expect(!WorkspaceShellTestAccess::IsSettingTransient(shell, "control.enabled"),
         "durably-set key should not be marked transient");
  Expect(WorkspaceShellTestAccess::GetSettingValue(shell, "debug.enabled") == "true",
         "transient value should be visible live");

  WorkspaceShellTestAccess::SaveUserConfig(shell);
  WorkspaceShellTestAccess::RestoreUserConfig(shell);

  Expect(WorkspaceShellTestAccess::GetSettingValue(shell, "control.enabled") == "true",
         "durable setting should survive the persist/restore round-trip");
  Expect(WorkspaceShellTestAccess::GetSettingValue(shell, "debug.enabled") != "true",
         "transient setting must not persist to the saved config");
}

void TestStartupOverridesAreTransient() {
  TemporaryDirectory temp;
  ScopedAppHomes homes(temp.path() / "state", temp.path() / "config");

  WorkspaceShell shell;
  microide::workspace::WorkspaceStartupOptions options;
  options.setting_overrides = {{"control.enabled", "true"}};
  shell.SetStartupOptions(std::move(options));
  shell.Initialize({});

  WorkspaceShellTestAccess::ApplyStartupSettingOverrides(shell);
  Expect(WorkspaceShellTestAccess::GetSettingValue(shell, "control.enabled") == "true",
         "startup override should apply live");
  Expect(WorkspaceShellTestAccess::IsSettingTransient(shell, "control.enabled"),
         "startup override should be transient");
}

void TestExplicitProjectWinsOverRestore() {
  TemporaryDirectory temp;
  const std::filesystem::path project_a = temp.path() / "projA";
  const std::filesystem::path project_b = temp.path() / "projB";
  WriteFile(project_a / "a.txt", "alpha\n");
  WriteFile(project_b / "b.txt", "beta\n");

  const std::filesystem::path home = temp.path() / "home";
  std::filesystem::create_directories(home);
  ScopedEnvVar scoped_home("HOME", home.string());
  ScopedAppHomes homes(temp.path() / "state", temp.path() / "config");

  {
    WorkspaceShell shell;
    Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_a, false, false),
           "project A should open");
    shell.Shutdown();
  }

  {
    WorkspaceShell shell;
    shell.Initialize({});
    const auto roots = WorkspaceShellTestAccess::ProjectRoots(shell);
    Expect(RootsContain(roots, project_a), "bare launch should restore the saved session");
  }

  {
    WorkspaceShell shell;
    microide::workspace::WorkspaceStartupOptions options;
    options.project_path = project_b;
    shell.SetStartupOptions(std::move(options));
    shell.Initialize(project_b);
    const auto roots = WorkspaceShellTestAccess::ProjectRoots(shell);
    Expect(RootsContain(roots, project_b), "explicit project B should be opened");
    Expect(!RootsContain(roots, project_a),
           "explicit project should win over the saved session");
  }
}

void TestForceStartedControlChannelSurvivesSettingChange() {
  TemporaryDirectory temp;
  ScopedAppHomes homes(temp.path() / "state", temp.path() / "config");
  std::filesystem::create_directories(temp.path() / "run");
  ScopedEnvVar xdg_runtime("XDG_RUNTIME_DIR", (temp.path() / "run").string());

  WorkspaceShell shell;
  microide::workspace::WorkspaceStartupOptions options;
  options.control_stdout = true;
  shell.SetStartupOptions(std::move(options));
  shell.Initialize({});
  shell.ForceStartControlChannel();
  Expect(WorkspaceShellTestAccess::IsControlChannelRunning(shell),
         "--control force-starts the control channel socket");

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "set-setting debug.enabled true"),
         "the live setting change applies");
  Expect(WorkspaceShellTestAccess::IsControlChannelRunning(shell),
         "the force-started control channel survives a live settings change");
}

int ConnectControlSocket(const std::filesystem::path& path) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    return -1;
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::snprintf(address.sun_path, sizeof(address.sun_path), "%s", path.c_str());
  if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

class ControlLineGenerator {
 public:
  ControlLineGenerator(std::uint64_t seed, std::vector<std::string> commands, std::vector<std::string> words)
      : state_(seed * 6364136223846793005ull + 1442695040888963407ull),
        commands_(std::move(commands)),
        words_(std::move(words)) {}

  std::uint64_t Next(std::uint64_t bound) {
    state_ = state_ * 6364136223846793005ull + 1442695040888963407ull;
    return (state_ >> 33) % bound;
  }

  std::string Line() {
    util::JsonObject request;
    const std::uint64_t id_kind = Next(10);
    if (id_kind < 6) {
      request["id"] = util::JsonValue(static_cast<std::int64_t>(Next(1000000)));
    } else if (id_kind == 6) {
      request["id"] = util::JsonValue("seven");
    } else if (id_kind == 7) {
      request["id"] = util::JsonValue(1.5);
    } else if (id_kind == 8) {
      request["id"] = util::JsonValue(static_cast<std::int64_t>(-1) - static_cast<std::int64_t>(Next(1u << 30)));
    }
    const std::uint64_t kind = Next(20);
    if (kind < 9) {
      std::string line = commands_[Next(commands_.size())];
      for (std::uint64_t arg = Next(3); arg > 0; --arg) {
        line += " " + words_[Next(words_.size())];
      }
      request["command"] = util::JsonValue(std::move(line));
    } else if (kind < 16) {
      const auto verbs = workspace::ControlQueryVerbs();
      std::string verb(verbs[Next(verbs.size())]);
      request["query"] = util::JsonValue(verb);
      const std::uint64_t args = Next(8);
      if (args == 1) {
        request["args"] = util::JsonValue(util::JsonObject{});
      } else if (args == 2) {
        util::JsonObject object;
        object["path"] = util::JsonValue(words_[Next(words_.size())]);
        object["recent"] = util::JsonValue(true);
        object["lines"] = util::JsonValue(static_cast<std::int64_t>(Next(5)) - 2);
        object["id"] = util::JsonValue(static_cast<std::int64_t>(Next(4)));
        request["args"] = util::JsonValue(std::move(object));
      } else if (args == 3) {
        request["args"] = util::JsonValue(util::JsonArray{util::JsonValue(1.0), util::JsonValue("x")});
      } else if (args == 4) {
        request["args"] = util::JsonValue("text");
      }
    } else if (kind == 16) {
      request["command"] = util::JsonValue("");
      request["query"] = util::JsonValue("tabs");
    } else if (kind == 17) {
      request["command"] = util::JsonValue(static_cast<std::int64_t>(3));
    } else {
      static const char* const kMalformed[] = {
          "{", "[]", "42", "\"query\"", "{\"query\":", "{\"query\":\"tabs\"}}", "{\"id\":1}", "null",
          "{\"query\":\"\\ud800\"}", "\xff\xfe{}", "{\"query\":\"tabs\",\"query\":\"editor\"}"};
      return kMalformed[Next(std::size(kMalformed))];
    }
    return util::SerializeJson(util::JsonValue(std::move(request)));
  }

 private:
  std::uint64_t state_;
  std::vector<std::string> commands_;
  std::vector<std::string> words_;
};

bool CommandIsSafeToFuzz(const std::string& command) {
  static const std::vector<std::string> kForbidden = {
      "quit", "project-open", "project-close", "open-file", "open-folder", "save-as", "key", "pointer",
      "restart", "set-setting"};
  static const std::vector<std::string> kForbiddenPrefixes = {"debug-", "tests-", "remote-", "breakpoint-",
                                                              "terminal-"};
  if (std::find(kForbidden.begin(), kForbidden.end(), command) != kForbidden.end()) {
    return false;
  }
  return std::none_of(kForbiddenPrefixes.begin(), kForbiddenPrefixes.end(),
                      [&](const std::string& prefix) { return command.starts_with(prefix); });
}

void TestEveryControlLineGetsOneReplyInOrder() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideoInitialized();
  TemporaryDirectory temp;
  ScopedAppHomes homes(temp.path() / "state", temp.path() / "config");
  std::filesystem::create_directories(temp.path() / "run");
  ScopedEnvVar xdg_runtime("XDG_RUNTIME_DIR", (temp.path() / "run").string());
  const std::filesystem::path root = temp.path() / "repo";
  WriteFile(root / "notes.txt", "alpha\nbeta\ngamma\n");
  WriteFile(root / "src" / "main.cpp", "int main() {\n  return 0;\n}\n");
  InitializeGitRepo(root);
  CommitAll(root, "base", "base");
  WriteFile(root / "notes.txt", "alpha\nchanged\n");

  constexpr int kWidth = 1200;
  constexpr int kHeight = 800;
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false), "the project opens");
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  WorkspaceShellTestAccess::OpenFile(shell, root / "notes.txt");
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "set-setting control.enabled true"),
         "the control channel is enabled");
  Expect(WorkspaceShellTestAccess::IsControlChannelRunning(shell), "the control channel starts");

  std::vector<std::string> commands;
  const util::JsonValue catalogue = WorkspaceShellTestAccess::ControlQuery(shell, "commands");
  for (const util::JsonValue& entry : catalogue.AsArray()) {
    const std::string& name = entry["command"].AsString();
    if (!name.empty() && !entry["plugin"].AsBool(false) && CommandIsSafeToFuzz(name)) {
      commands.push_back(name);
    }
  }
  Expect(commands.size() > 50, "the catalogue lists the commands to fuzz, " + std::to_string(commands.size()));
  const std::vector<std::string> words = {
      "notes.txt", (root / "notes.txt").string(), "src/main.cpp", "1", "0", "-3", "99999999999999999999",
      "true", "--force", "a b", "\"quoted\"", "\xc3\xa9t\xc3\xa9", std::string(300, 'w'), "HEAD", "../outside",
      "*", "%s%n", "main"};

  const std::filesystem::path socket = temp.path() / "run" / "microide" / (std::to_string(::getpid()) + ".sock");
  SoftwareCanvas canvas(kWidth, kHeight);
  for (const std::uint64_t seed : {1u, 2u, 3u}) {
    ControlLineGenerator generator(seed, commands, words);
    std::vector<int> fds;
    for (int connection = 0; connection < 3; ++connection) {
      fds.push_back(ConnectControlSocket(socket));
      Expect(fds.back() >= 0, "a client connects to the control socket");
    }
    std::vector<std::vector<std::string>> sent(fds.size());
    std::vector<std::string> received(fds.size());
    const auto pump = [&] {
      WorkspaceShellTestAccess::ConsumeControlCallbacks(shell);
      for (std::size_t c = 0; c < fds.size(); ++c) {
        char buffer[65536];
        ssize_t count = 0;
        while ((count = ::recv(fds[c], buffer, sizeof(buffer), MSG_DONTWAIT)) > 0) {
          received[c].append(buffer, static_cast<std::size_t>(count));
        }
      }
    };
    const auto replies = [&](std::size_t c) {
      return static_cast<std::size_t>(std::count(received[c].begin(), received[c].end(), '\n'));
    };
    for (int step = 0; step < 120; ++step) {
      const std::size_t c = generator.Next(fds.size());
      std::string payload;
      for (std::uint64_t n = 1 + generator.Next(3); n > 0; --n) {
        std::string line = generator.Line();
        payload += line + (generator.Next(4) == 0 ? "\r\n" : "\n");
        sent[c].push_back(std::move(line));
      }
      const std::size_t split = generator.Next(payload.size() + 1);
      Expect(::send(fds[c], payload.data(), split, MSG_NOSIGNAL) == static_cast<ssize_t>(split),
             "the first part of a request batch is sent");
      pump();
      Expect(::send(fds[c], payload.data() + split, payload.size() - split, MSG_NOSIGNAL) ==
                 static_cast<ssize_t>(payload.size() - split),
             "the rest of a request batch is sent");
      const bool answered = WaitUntil(
          [&] {
            pump();
            return replies(c) >= sent[c].size();
          },
          std::chrono::seconds(10), std::chrono::milliseconds(2));
      Expect(answered, "seed " + std::to_string(seed) + " step " + std::to_string(step) + ": the batch ending in " +
                           sent[c].back() + " is answered");
      if (generator.Next(4) == 0) {
        shell.Render(canvas.renderer(), kWidth, kHeight);
        (void)shell.ConsumePendingRenderInvalidation();
      }
    }
    pump();
    for (std::size_t c = 0; c < fds.size(); ++c) {
      std::vector<std::string> lines;
      std::size_t start = 0;
      for (std::size_t end = received[c].find('\n'); end != std::string::npos;
           start = end + 1, end = received[c].find('\n', start)) {
        lines.push_back(received[c].substr(start, end - start));
      }
      Expect(lines.size() == sent[c].size(), "seed " + std::to_string(seed) + ": connection " + std::to_string(c) +
                                                 " gets one reply per request line, " + std::to_string(lines.size()) +
                                                 " for " + std::to_string(sent[c].size()));
      for (std::size_t i = 0; i < std::min(lines.size(), sent[c].size()); ++i) {
        const std::optional<util::JsonValue> reply = util::ParseJson(lines[i]);
        Expect(reply.has_value() && reply->IsObject(), "every reply is a JSON object: " + lines[i].substr(0, 200));
        const workspace::ControlRequest request = workspace::ParseControlRequest(sent[c][i]);
        const bool id_matches = request.id.has_value() ? (*reply)["id"].AsInt(-1) == *request.id
                                                       : !(*reply).HasKey("id") || (*reply)["id"].IsNull();
        Expect(id_matches, "reply " + std::to_string(i) + " answers its own request: " + sent[c][i].substr(0, 200) +
                               " -> " + lines[i].substr(0, 200));
      }
      ::close(fds[c]);
    }
  }
  const util::JsonValue invariants = WorkspaceShellTestAccess::ControlQuery(shell, "invariants");
  Expect(invariants["ok"].AsBool(false), "the shell's invariants hold after the fuzzed lines: " +
                                            util::SerializeJson(invariants).substr(0, 400));
}

}

void TestRestoredSettingsAreCanonicalized() {
  TemporaryDirectory temp;
  ScopedAppHomes homes(temp.path() / "state", temp.path() / "config");
  {
    WorkspaceShell writer;
    writer.Initialize({});
    WorkspaceShellTestAccess::SetRawUserSetting(writer, "debug.enabled", "no");
    WorkspaceShellTestAccess::SetRawUserSetting(writer, "editor.fold.enabled", "On");
    WorkspaceShellTestAccess::SetRawUserSetting(writer, "editor.font_size", "999");
    WorkspaceShellTestAccess::SetRawUserSetting(writer, "lsp.enabled", "maybe");
    WorkspaceShellTestAccess::SetRawUserSetting(writer, "someplugin.flag", "no");
    WorkspaceShellTestAccess::SaveUserConfig(writer);
  }
  WorkspaceShell shell;
  shell.Initialize({});
  const auto value = [&](std::string_view id) {
    return WorkspaceShellTestAccess::GetSettingValue(shell, id).value_or("<unset>");
  };
  Expect(value("debug.enabled") == "false", "a hand-edited \"no\" loads as false: " + value("debug.enabled"));
  Expect(value("editor.fold.enabled") == "true", "\"On\" loads as true: " + value("editor.fold.enabled"));
  Expect(value("editor.font_size") != "999", "an int loads clamped to its range: " + value("editor.font_size"));
  Expect(value("lsp.enabled") == "maybe", "a value that does not parse is kept as written");
  Expect(value("someplugin.flag") == "no", "a setting no built-in spec names is kept as written");
}

void TestRestoredExcludeGlobsDoNotRescanOnFirstLiveSettingsPass() {
  TemporaryDirectory temp;
  ScopedAppHomes homes(temp.path() / "state", temp.path() / "config");
  const std::filesystem::path root = temp.path() / "project";
  WriteFile(root / "README.md", "root\n");
  WriteFile(root / "src" / "keep.cpp", "int keep() { return 1; }\n");
  WriteFile(root / "vendor" / "skip.cpp", "int skip() { return 2; }\n");

  {
    WorkspaceShell writer;
    writer.Initialize({});
    Expect(WorkspaceShellTestAccess::SetSettingValue(writer, "project.files_exclude", "vendor/"),
           "the exclude-glob setting should accept a value");
    WorkspaceShellTestAccess::SaveUserConfig(writer);
  }

  WorkspaceShell shell;
  Expect(shell.Initialize(root), "a fresh shell should restore the config and open the project");
  Expect(WorkspaceShellTestAccess::GetSettingValue(shell, "project.files_exclude") == "vendor/",
         "the restored config should carry the exclude glob");

  util::ResetPerformanceCounters();
  WorkspaceShellTestAccess::ApplyLiveSettings(shell);
  Expect(util::ReadPerformanceCounter(util::PerfCounterId::FileIndexRefreshRequests) == 0,
         "the first live-settings pass must not rescan for exclude globs the open already applied");
  Expect(util::ReadPerformanceCounter(util::PerfCounterId::FileIndexWatcherStarts) == 0,
         "the first live-settings pass must not re-arm the file index watcher");

  Expect(WorkspaceShellTestAccess::SetSettingValueTransient(shell, "project.files_exclude",
                                                            "vendor/\nout/"),
         "the exclude-glob setting should accept an edited value");
  Expect(util::ReadPerformanceCounter(util::PerfCounterId::FileIndexRefreshRequests) >= 1,
         "an actual exclude-glob edit should still request a rescan");
  Expect(util::ReadPerformanceCounter(util::PerfCounterId::FileIndexWatcherStarts) >= 1,
         "an actual exclude-glob edit should still re-arm the watcher");
}

void TestControlCommandRepeatsTheSameRejectionMessage() {
  TemporaryDirectory temp;
  ScopedAppHomes homes(temp.path() / "state", temp.path() / "config");

  WorkspaceShell shell;
  shell.Initialize({});

  const auto first = WorkspaceShellTestAccess::ExecuteControlCommand(shell, "project-next");
  Expect(!first.ok, "project-next with no project open should be rejected");
  Expect(first.error == "No active project",
         "the first rejection should carry its message: " + first.error);
  const auto second = WorkspaceShellTestAccess::ExecuteControlCommand(shell, "project-prev");
  Expect(!second.ok, "project-prev with no project open should be rejected");
  Expect(second.error == first.error,
         "a rejection repeating the previous message must not read as 'command failed': " +
             second.error);

  const auto ok = WorkspaceShellTestAccess::ExecuteControlCommand(shell, "set-setting control.enabled true");
  Expect(ok.ok && ok.feedback.empty(), "a silent success reports no feedback: " + ok.feedback);
}

void RegisterWorkspaceShellControlSettingsTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceShellControlSettings/EveryControlLineGetsOneReplyInOrder",
          TestEveryControlLineGetsOneReplyInOrder);
  AddTest(tests, "WorkspaceShellControlSettings/ControlCommandRepeatsTheSameRejectionMessage",
          TestControlCommandRepeatsTheSameRejectionMessage);
  AddTest(tests, "WorkspaceShellControlSettings/ForceStartedControlChannelSurvivesSettingChange",
          TestForceStartedControlChannelSurvivesSettingChange);
  AddTest(tests, "WorkspaceShellControlSettings/SetSettingCommandFlipsAndRejects",
          TestSetSettingCommandFlipsAndRejects);
  AddTest(tests, "WorkspaceShellControlSettings/SetSettingStoresClampedBuiltinValue",
          TestSetSettingStoresClampedBuiltinValue);
  AddTest(tests, "WorkspaceShellControlSettings/SettingsStepperStopsAtTheBounds",
          TestSettingsStepperStopsAtTheBounds);
  AddTest(tests, "WorkspaceShellControlSettings/TransientSettingNotPersisted",
          TestTransientSettingNotPersisted);
  AddTest(tests, "WorkspaceShellControlSettings/StartupOverridesAreTransient",
          TestStartupOverridesAreTransient);
  AddTest(tests, "WorkspaceShellControlSettings/RestoredSettingsAreCanonicalized",
          TestRestoredSettingsAreCanonicalized);
  AddTest(tests, "WorkspaceShellControlSettings/ExplicitProjectWinsOverRestore",
          TestExplicitProjectWinsOverRestore);
  AddTest(tests, "WorkspaceShellControlSettings/RestoredExcludeGlobsDoNotRescanOnFirstLiveSettingsPass",
          TestRestoredExcludeGlobsDoNotRescanOnFirstLiveSettingsPass);
}

}
