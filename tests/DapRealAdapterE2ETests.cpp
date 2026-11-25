#include "TestSupport.h"
#include "parity/LoopbackLocality.h"
#include "project/remote/RemoteProcessLauncher.h"

#include "platform/Subprocess.h"
#include "util/JsonValue.h"
#include "workspace/debug/WorkspaceDapClient.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::DapClient;
namespace dap_protocol = microide::workspace::dap_protocol;

std::string LocateGdb() {
  if (const char* override_path = std::getenv("MICROIDE_TEST_DAP_GDB");
      override_path != nullptr && override_path[0] != '\0') {
    std::error_code ec;
    if (std::filesystem::exists(override_path, ec)) {
      return override_path;
    }
    return {};
  }
  const char* path_env = std::getenv("PATH");
  if (path_env == nullptr) {
    return {};
  }
  const std::string path = path_env;
  std::size_t start = 0;
  while (start <= path.size()) {
    const std::size_t colon = path.find(':', start);
    const std::string dir =
        path.substr(start, colon == std::string::npos ? std::string::npos : colon - start);
    if (!dir.empty()) {
      std::error_code ec;
      const std::filesystem::path candidate = std::filesystem::path(dir) / "gdb";
      if (std::filesystem::exists(candidate, ec) && !std::filesystem::is_directory(candidate, ec)) {
        return candidate.string();
      }
    }
    if (colon == std::string::npos) {
      break;
    }
    start = colon + 1;
  }
  return {};
}

std::string LocateCCompiler() {
  if (const char* override_path = std::getenv("MICROIDE_TEST_CC");
      override_path != nullptr && override_path[0] != '\0') {
    std::error_code ec;
    return std::filesystem::exists(override_path, ec) ? override_path : std::string{};
  }
  const char* path_env = std::getenv("PATH");
  if (path_env == nullptr) {
    return {};
  }
  const std::string path = path_env;
  for (const char* name : {"cc", "gcc", "clang"}) {
    std::size_t start = 0;
    while (start <= path.size()) {
      const std::size_t colon = path.find(':', start);
      const std::string dir =
          path.substr(start, colon == std::string::npos ? std::string::npos : colon - start);
      if (!dir.empty()) {
        std::error_code ec;
        const std::filesystem::path candidate = std::filesystem::path(dir) / name;
        if (std::filesystem::exists(candidate, ec) &&
            !std::filesystem::is_directory(candidate, ec)) {
          return candidate.string();
        }
      }
      if (colon == std::string::npos) {
        break;
      }
      start = colon + 1;
    }
  }
  return {};
}

bool CompileDebuggee(const std::string& compiler,
                     const std::filesystem::path& source,
                     const std::filesystem::path& program) {
  const platform::SubprocessResult result = platform::RunSubprocess(
      {compiler, "-g", "-O0", "-o", program.string(), source.string()}, {});
  std::error_code ec;
  return result.exit_code == 0 && std::filesystem::exists(program, ec);
}

template <typename Predicate>
bool PumpUntil(DapClient& client, Predicate&& ready, int timeout_ms) {
  return WaitUntil([&ready]() { return ready(); }, std::chrono::milliseconds(timeout_ms),
                   std::chrono::milliseconds(20),
                   [&client]() { client.DrainCallbacks(); });
}

void TestDapRealAdapterGdbHandshakeAndShutdown() {
  const std::string gdb = LocateGdb();
  if (gdb.empty()) {
    std::fprintf(stderr,
                 "[dap-e2e] SKIP: no gdb on PATH (set MICROIDE_TEST_DAP_GDB to enable the "
                 "real-adapter end-to-end suite)\n");
    return;
  }

  DapClient client;
  client.SetWakeChannel(0);

  std::mutex event_mutex;
  std::vector<std::string> events;
  client.SetEventCallback([&](const std::string& event, const util::JsonValue&) {
    std::lock_guard lock(event_mutex);
    events.push_back(event);
  });

  if (!client.Start(platform::LocalProcessLauncher(), {gdb, "--interpreter=dap"}, "gdb")) {
    std::fprintf(stderr, "[dap-e2e] SKIP: gdb could not be started as a DAP adapter\n");
    return;
  }

  if (!PumpUntil(client, [&]() { return client.IsInitialized() || !client.IsRunning(); }, 15000)) {
    client.Shutdown();
    std::fprintf(stderr, "[dap-e2e] SKIP: gdb did not answer initialize in time\n");
    return;
  }
  if (!client.IsInitialized()) {
    client.Shutdown();
    std::fprintf(stderr,
                 "[dap-e2e] SKIP: gdb exited without completing initialize (needs gdb >= 14 for "
                 "--interpreter=dap)\n");
    return;
  }

  Expect(client.IsRunning(), "the adapter process should still be running after initialize");

  const auto capabilities = client.Capabilities();
  Expect(capabilities.supports_configuration_done_request,
         "gdb should advertise supportsConfigurationDoneRequest");
  Expect(capabilities.supports_conditional_breakpoints,
         "gdb should advertise supportsConditionalBreakpoints");

  bool threads_answered = false;
  bool threads_success = false;
  Expect(client.SendRequestAsync("threads", util::JsonValue(nullptr),
                                 [&](const dap_protocol::DapResponse& response) {
                                   threads_answered = true;
                                   threads_success = response.success;
                                 }),
         "sending a request to a live adapter should be accepted");
  Expect(PumpUntil(client, [&]() { return threads_answered; }, 10000),
         "gdb should answer a `threads` request over the real transport");
  Expect(threads_success, "gdb's `threads` response should report success");

  {
    std::lock_guard lock(event_mutex);
    Expect(!events.empty(), "gdb should have pushed at least one adapter event");
  }

  client.Shutdown();
  Expect(PumpUntil(client, [&]() { return !client.IsRunning(); }, 10000),
         "the adapter process should exit after Shutdown()");
}

struct GdbCycleResult {
  bool started = false;
  bool stopped = false;
  std::string frame_path;
  std::int64_t frame_line = 0;
};

GdbCycleResult RunGdbBreakpointCycle(const std::string& gdb,
                                     const platform::ProcessLauncher& launcher,
                                     const std::filesystem::path& cwd,
                                     const std::filesystem::path& source,
                                     const std::filesystem::path& program) {
  GdbCycleResult result;
  DapClient client;
  client.SetWakeChannel(0);
  std::mutex event_mutex;
  bool saw_stopped = false;
  client.SetEventCallback([&](const std::string& event, const util::JsonValue&) {
    std::lock_guard lock(event_mutex);
    saw_stopped = saw_stopped || event == "stopped";
  });
  if (!client.Start(launcher, {gdb, "--interpreter=dap"}, "gdb", cwd.string()) ||
      !PumpUntil(client, [&]() { return client.IsInitialized() || !client.IsRunning(); }, 15000) ||
      !client.IsInitialized()) {
    client.Shutdown();
    return result;
  }
  result.started = true;
  util::JsonObject breakpoint_line;
  breakpoint_line["line"] = util::JsonValue(static_cast<std::int64_t>(2));
  util::JsonObject source_ref;
  source_ref["path"] = util::JsonValue(source.string());
  util::JsonObject set_breakpoints_args;
  set_breakpoints_args["source"] = util::JsonValue(std::move(source_ref));
  set_breakpoints_args["breakpoints"] =
      util::JsonValue(util::JsonArray{util::JsonValue(std::move(breakpoint_line))});
  bool breakpoints_answered = false;
  (void)client.SendRequestAsync("setBreakpoints", util::JsonValue(std::move(set_breakpoints_args)),
                                [&](const dap_protocol::DapResponse&) { breakpoints_answered = true; });
  (void)PumpUntil(client, [&]() { return breakpoints_answered; }, 10000);
  util::JsonObject launch_args;
  launch_args["program"] = util::JsonValue(program.string());
  (void)client.SendRequestAsync("launch", util::JsonValue(std::move(launch_args)), {});
  bool configuration_done_answered = false;
  (void)client.SendRequestAsync("configurationDone", util::JsonValue(nullptr),
                                [&](const dap_protocol::DapResponse&) {
                                  configuration_done_answered = true;
                                });
  (void)PumpUntil(client, [&]() { return configuration_done_answered; }, 20000);
  (void)PumpUntil(client, [&]() { return saw_stopped || !client.IsRunning(); }, 30000);
  result.stopped = saw_stopped;
  if (result.stopped) {
    util::JsonObject stack_args;
    stack_args["threadId"] = util::JsonValue(static_cast<std::int64_t>(1));
    bool stack_answered = false;
    (void)client.SendRequestAsync("stackTrace", util::JsonValue(std::move(stack_args)),
                                  [&](const dap_protocol::DapResponse& response) {
                                    stack_answered = true;
                                    const util::JsonValue& frames = response.body["stackFrames"];
                                    if (frames.IsArray() && !frames.AsArray().empty()) {
                                      result.frame_path =
                                          frames[std::size_t{0}]["source"]["path"].AsString();
                                      result.frame_line = frames[std::size_t{0}]["line"].AsInt();
                                    }
                                  });
    (void)PumpUntil(client, [&]() { return stack_answered; }, 10000);
  }
  client.Shutdown();
  return result;
}

void TestDapRealAdapterGdbStopsInTheMirrorThroughALoopbackHost() {
  const std::string gdb = LocateGdb();
  const std::string compiler = LocateCCompiler();
  if (gdb.empty() || compiler.empty()) {
    std::fprintf(stderr, "[dap-e2e] SKIP: gdb or a C compiler is missing\n");
    return;
  }
  TemporaryDirectory temp_dir;
  const std::filesystem::path host = temp_dir.path() / "host" / "project";
  const std::filesystem::path mirror = temp_dir.path() / "mirror" / "project";
  const std::string source_text =
      "#include <stdio.h>\n"
      "int add(int a, int b) { int sum = a + b; return sum; }\n"
      "int main(void) { printf(\"%d\\n\", add(2, 3)); return 0; }\n";
  WriteFile(host / "debuggee.c", source_text);
  WriteFile(mirror / "debuggee.c", source_text);
  if (!CompileDebuggee(compiler, host / "debuggee.c", host / "debuggee")) {
    std::fprintf(stderr, "[dap-e2e] SKIP: could not compile the debuggee with -g\n");
    return;
  }

  const GdbCycleResult reference = RunGdbBreakpointCycle(
      gdb, platform::LocalProcessLauncher(), host, host / "debuggee.c", host / "debuggee");
  if (!reference.stopped) {
    std::fprintf(stderr, "[dap-e2e] SKIP: gdb cannot stop an inferior here (ptrace?)\n");
    return;
  }

  const tests::parity::LoopbackPathMap map(mirror, host);
  const tests::parity::LoopbackProcessLauncher loopback(map);
  std::string error;
  const auto server = tests::parity::ConnectServerLocality(map, &error);
  Expect(server != nullptr, "the server locality connects: " + error);
  const std::pair<const platform::ProcessLauncher*, std::string_view> localities[] = {
      {&loopback, "loopback"}, {server.get(), "server"}};
  for (const auto& [launcher, name] : localities) {
    if (launcher == nullptr) {
      continue;
    }
    const std::string where = " (" + std::string(name) + ")";
    const GdbCycleResult remote = RunGdbBreakpointCycle(
        gdb, *launcher, mirror, mirror / "debuggee.c", mirror / "debuggee");
    Expect(remote.started, "gdb starts through the non-local host" + where);
    Expect(remote.stopped,
           "the MIRROR's program and breakpoint reach the host's binary, and it stops there" +
               where);
    Expect(std::filesystem::path(remote.frame_path).lexically_normal() ==
                   (mirror / "debuggee.c").lexically_normal() &&
               remote.frame_line == 2,
           "the stop is reported in the MIRROR's file, on the breakpoint's line; got " +
               remote.frame_path + ":" + std::to_string(remote.frame_line) + where);
  }
}

void TestDapRealAdapterGdbLaunchBreakpointStopCycle() {
  const std::string gdb = LocateGdb();
  if (gdb.empty()) {
    std::fprintf(stderr, "[dap-e2e] SKIP: no gdb on PATH for the launch cycle\n");
    return;
  }
  const std::string compiler = LocateCCompiler();
  if (compiler.empty()) {
    std::fprintf(stderr, "[dap-e2e] SKIP: no C compiler on PATH to build a debuggee\n");
    return;
  }

  TemporaryDirectory temp_dir;
  const std::filesystem::path source = temp_dir.path() / "debuggee.c";
  const std::filesystem::path program = temp_dir.path() / "debuggee";
  WriteFile(source,
            "#include <stdio.h>\n"
            "int add(int a, int b) { int sum = a + b; return sum; }\n"
            "int main(void) { printf(\"%d\\n\", add(2, 3)); return 0; }\n");
  if (!CompileDebuggee(compiler, source, program)) {
    std::fprintf(stderr, "[dap-e2e] SKIP: could not compile the debuggee with -g\n");
    return;
  }

  DapClient client;
  client.SetWakeChannel(0);

  std::mutex event_mutex;
  std::vector<std::string> event_names;
  bool saw_stopped = false;
  bool saw_exited = false;
  bool breakpoint_verified_by_event = false;
  client.SetEventCallback([&](const std::string& event, const util::JsonValue& body) {
    std::lock_guard lock(event_mutex);
    event_names.push_back(event);
    if (event == "stopped") {
      saw_stopped = true;
    } else if (event == "exited") {
      saw_exited = true;
    } else if (event == "breakpoint" && body["breakpoint"]["verified"].AsBool(false)) {
      breakpoint_verified_by_event = true;
    }
  });

  if (!client.Start(platform::LocalProcessLauncher(), {gdb, "--interpreter=dap"}, "gdb")) {
    std::fprintf(stderr, "[dap-e2e] SKIP: gdb could not be started as a DAP adapter\n");
    return;
  }
  if (!PumpUntil(client, [&]() { return client.IsInitialized() || !client.IsRunning(); }, 15000) ||
      !client.IsInitialized()) {
    client.Shutdown();
    std::fprintf(stderr, "[dap-e2e] SKIP: gdb did not complete initialize (needs gdb >= 14)\n");
    return;
  }

  util::JsonObject breakpoint_line;
  breakpoint_line["line"] = util::JsonValue(static_cast<std::int64_t>(2));
  util::JsonObject source_ref;
  source_ref["path"] = util::JsonValue(source.string());
  util::JsonObject set_breakpoints_args;
  set_breakpoints_args["source"] = util::JsonValue(std::move(source_ref));
  set_breakpoints_args["breakpoints"] =
      util::JsonValue(util::JsonArray{util::JsonValue(std::move(breakpoint_line))});

  bool breakpoints_answered = false;
  bool breakpoint_reported_pending = false;
  Expect(client.SendRequestAsync("setBreakpoints", util::JsonValue(std::move(set_breakpoints_args)),
                                 [&](const dap_protocol::DapResponse& response) {
                                   breakpoints_answered = true;
                                   const util::JsonValue& list = response.body["breakpoints"];
                                   if (list.IsArray() && !list.AsArray().empty()) {
                                     breakpoint_reported_pending =
                                         !list[std::size_t{0}]["verified"].AsBool(false);
                                   }
                                 }),
         "setBreakpoints should be accepted by a live adapter");
  Expect(PumpUntil(client, [&]() { return breakpoints_answered; }, 10000),
         "gdb should answer setBreakpoints before the program is loaded");
  Expect(breakpoint_reported_pending,
         "a breakpoint set before the module loads must report unverified, not verified");

  util::JsonObject launch_args;
  launch_args["program"] = util::JsonValue(program.string());
  Expect(client.SendRequestAsync("launch", util::JsonValue(std::move(launch_args)), {}),
         "launch should be accepted by a live adapter");
  bool configuration_done_answered = false;
  bool configuration_done_success = false;
  Expect(client.SendRequestAsync("configurationDone", util::JsonValue(nullptr),
                                 [&](const dap_protocol::DapResponse& response) {
                                   configuration_done_answered = true;
                                   configuration_done_success = response.success;
                                 }),
         "configurationDone should be accepted by a live adapter");
  Expect(PumpUntil(client, [&]() { return configuration_done_answered; }, 20000),
         "gdb should answer configurationDone");

  if (!configuration_done_success ||
      !PumpUntil(client, [&]() { return saw_stopped || !client.IsRunning(); }, 30000) ||
      !saw_stopped) {
    client.Shutdown();
    std::fprintf(stderr,
                 "[dap-e2e] SKIP: gdb could not run an inferior to the breakpoint (no "
                 "CAP_SYS_PTRACE / ptrace_scope restricted?)\n");
    return;
  }

  Expect(breakpoint_verified_by_event,
         "the pending breakpoint must be re-reported as verified once its module loads");

  util::JsonObject stack_args;
  stack_args["threadId"] = util::JsonValue(static_cast<std::int64_t>(1));
  bool stack_answered = false;
  std::string top_frame_name;
  std::int64_t top_frame_line = 0;
  std::int64_t top_frame_id = 0;
  Expect(client.SendRequestAsync("stackTrace", util::JsonValue(std::move(stack_args)),
                                 [&](const dap_protocol::DapResponse& response) {
                                   stack_answered = true;
                                   const util::JsonValue& frames = response.body["stackFrames"];
                                   if (frames.IsArray() && !frames.AsArray().empty()) {
                                     const util::JsonValue& top = frames[std::size_t{0}];
                                     top_frame_name = top["name"].AsString();
                                     top_frame_line = top["line"].AsInt();
                                     top_frame_id = top["id"].AsInt();
                                   }
                                 }),
         "stackTrace should be accepted while stopped");
  Expect(PumpUntil(client, [&]() { return stack_answered; }, 10000),
         "gdb should answer stackTrace while stopped");
  Expect(top_frame_name == "add",
         "the stop should be inside the function the breakpoint is in, not wherever the "
         "program happened to be: " + top_frame_name);
  Expect(top_frame_line == 2,
         "the stop should be on the breakpoint's line, got " + std::to_string(top_frame_line));

  util::JsonObject scopes_args;
  scopes_args["frameId"] = util::JsonValue(top_frame_id);
  bool scopes_answered = false;
  std::int64_t arguments_reference = 0;
  Expect(client.SendRequestAsync("scopes", util::JsonValue(std::move(scopes_args)),
                                 [&](const dap_protocol::DapResponse& response) {
                                   scopes_answered = true;
                                   const util::JsonValue& scopes = response.body["scopes"];
                                   if (!scopes.IsArray()) {
                                     return;
                                   }
                                   for (const util::JsonValue& scope : scopes.AsArray()) {
                                     if (scope["name"].AsString() == "Arguments") {
                                       arguments_reference =
                                           scope["variablesReference"].AsInt();
                                     }
                                   }
                                 }),
         "scopes should be accepted while stopped");
  Expect(PumpUntil(client, [&]() { return scopes_answered; }, 10000),
         "gdb should answer scopes for a live frame");
  Expect(arguments_reference != 0, "a stopped frame should expose an Arguments scope");

  util::JsonObject variables_args;
  variables_args["variablesReference"] = util::JsonValue(arguments_reference);
  bool variables_answered = false;
  std::string a_value;
  std::string b_value;
  Expect(client.SendRequestAsync("variables", util::JsonValue(std::move(variables_args)),
                                 [&](const dap_protocol::DapResponse& response) {
                                   variables_answered = true;
                                   const util::JsonValue& variables = response.body["variables"];
                                   if (!variables.IsArray()) {
                                     return;
                                   }
                                   for (const util::JsonValue& variable : variables.AsArray()) {
                                     if (variable["name"].AsString() == "a") {
                                       a_value = variable["value"].AsString();
                                     } else if (variable["name"].AsString() == "b") {
                                       b_value = variable["value"].AsString();
                                     }
                                   }
                                 }),
         "variables should be accepted for a live scope");
  Expect(PumpUntil(client, [&]() { return variables_answered; }, 10000),
         "gdb should answer variables for a live scope");
  Expect(a_value == "2" && b_value == "3",
         "the inferior's real argument values should come back, got a=" + a_value +
             " b=" + b_value);

  util::JsonObject continue_args;
  continue_args["threadId"] = util::JsonValue(static_cast<std::int64_t>(1));
  Expect(client.SendRequestAsync("continue", util::JsonValue(std::move(continue_args)), {}),
         "continue should be accepted while stopped");
  Expect(PumpUntil(client, [&]() { return saw_exited || !client.IsRunning(); }, 20000),
         "the inferior should run to completion and report `exited` after continue");
  Expect(saw_exited, "resuming past the only breakpoint should end in an `exited` event");

  client.Shutdown();
  Expect(PumpUntil(client, [&]() { return !client.IsRunning(); }, 10000),
         "the adapter process should exit after Shutdown()");
}

}

void RegisterDapRealAdapterE2ETests(std::vector<TestCase>& tests) {
  AddTest(tests, "DapRealAdapter/GdbHandshakeAndShutdown",
          TestDapRealAdapterGdbHandshakeAndShutdown);
  AddTest(tests, "DapRealAdapter/GdbStopsInTheMirrorThroughALoopbackHost",
          TestDapRealAdapterGdbStopsInTheMirrorThroughALoopbackHost);
  AddTest(tests, "DapRealAdapter/GdbLaunchBreakpointStopCycle",
          TestDapRealAdapterGdbLaunchBreakpointStopCycle);
}

}
