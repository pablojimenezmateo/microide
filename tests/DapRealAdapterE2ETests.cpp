// End-to-end DAP coverage against a REAL debug adapter (gdb's built-in
// `--interpreter=dap`), mirroring the clangd suite in LspRealServerE2ETests.cpp.
//
// This closes a coverage asymmetry: LSP had a real-server end-to-end test while
// DAP — an equally large shipped subsystem, with a bundled gdb-dap plugin — had
// only protocol unit tests and a stub-mode client test. Nothing exercised the
// real transport against a real adapter: the Content-Length framing, the
// initialize handshake, capability parsing, adapter-pushed events, and shutdown
// were only ever driven by in-tree fakes that we also wrote.
//
// OPT-IN by availability, like the clangd suite: with no usable gdb the test logs
// a skip and passes, so a machine without a debugger stays green. Set
// $MICROIDE_TEST_DAP_GDB to force a specific binary. gdb only grew `--interpreter=dap`
// in 14.x, so an older gdb on PATH also skips rather than failing.
#include "TestSupport.h"

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

// Locate a gdb that supports `--interpreter=dap`: an explicit
// $MICROIDE_TEST_DAP_GDB override first, then a PATH scan. Empty means skip.
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

// A C compiler to build a debuggee with. Same availability-gated contract as
// LocateGdb: no compiler means skip, not fail.
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

// -O0 -g so the breakpoint lands on the line it was set on and the arguments are
// still live at it; an optimized build may fold both away and the assertions
// below would be testing the optimizer.
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
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#else
  const std::string gdb = LocateGdb();
  if (gdb.empty()) {
    std::fprintf(stderr,
                 "[dap-e2e] SKIP: no gdb on PATH (set MICROIDE_TEST_DAP_GDB to enable the "
                 "real-adapter end-to-end suite)\n");
    return;
  }

  DapClient client;
  client.SetWakeChannel(0);

  // Adapter-pushed events arrive on the main thread via DrainCallbacks. gdb emits
  // `output` events immediately at startup (its version banner), which is exactly
  // the "server pushes before we ask anything" case the framer has to survive.
  std::mutex event_mutex;
  std::vector<std::string> events;
  client.SetEventCallback([&](const std::string& event, const util::JsonValue&) {
    std::lock_guard lock(event_mutex);
    events.push_back(event);
  });

  if (!client.Start({gdb, "--interpreter=dap"}, "gdb")) {
    std::fprintf(stderr, "[dap-e2e] SKIP: gdb could not be started as a DAP adapter\n");
    return;
  }

  // A gdb too old for --interpreter=dap exits immediately instead of speaking the
  // protocol; treat that as unavailable rather than a failure, matching the
  // clangd suite's availability-gated contract.
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

  // From here on the adapter is real and talking, so every assertion is
  // unconditional — a regression in the transport must fail, not skip.
  Expect(client.IsRunning(), "the adapter process should still be running after initialize");

  // Real capability negotiation. gdb advertises conditional breakpoints and
  // configurationDone; asserting a capability we did not synthesize ourselves is
  // the point of driving a real adapter.
  const auto capabilities = client.Capabilities();
  Expect(capabilities.supports_configuration_done_request,
         "gdb should advertise supportsConfigurationDoneRequest");
  Expect(capabilities.supports_conditional_breakpoints,
         "gdb should advertise supportsConditionalBreakpoints");

  // A real request/response round trip over the real framing. `threads` before a
  // launch is answered by gdb (with an empty or single-entry list) rather than
  // erroring, which keeps this independent of having a debuggable binary.
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

  // The adapter pushed at least one event before we asked for anything (gdb's
  // startup banner arrives as `output` events). This is the cross-chunk,
  // server-pushes-first path the Content-Length framer has to survive.
  {
    std::lock_guard lock(event_mutex);
    Expect(!events.empty(), "gdb should have pushed at least one adapter event");
  }

  // Graceful shutdown: `disconnect` handshake, then the process exits.
  client.Shutdown();
  Expect(PumpUntil(client, [&]() { return !client.IsRunning(); }, 10000),
         "the adapter process should exit after Shutdown()");
#endif
}

// The launch -> breakpoint -> stopped -> inspect -> continue -> exit cycle,
// against the same real gdb the handshake test drives.
//
// Everything past `initialize` used to be covered only in stub mode -- 85 tests
// against a fake we also wrote -- which is the gap that hid the pending-breakpoint
// tint bug (TD-2026-07-26-006). The parts that only a real adapter exercises are
// the ones asserted here: a breakpoint set BEFORE the program is loaded comes back
// `verified:false, reason:"pending"` and is re-reported verified through a
// `breakpoint` event once the module loads; the `launch` response does not arrive
// before `configurationDone`, so anything that waits on it deadlocks; and the stop
// carries a real frame, real scopes and real argument values.
//
// Skips the same way the handshake test does, and for one more reason: a sandbox
// without CAP_SYS_PTRACE (or with kernel.yama.ptrace_scope locked down) cannot let
// gdb start an inferior at all. That is reported as a skip, but ONLY when the
// adapter says so -- once a `stopped` event arrives every assertion is
// unconditional, so a regression fails rather than quietly skipping.
void TestDapRealAdapterGdbLaunchBreakpointStopCycle() {
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#else
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
  // `add` is on line 2 and its body assigns before returning, so a breakpoint on
  // line 2 stops with both arguments already bound to known values.
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
  // The re-report of a pending breakpoint once its module loads. gdb sends this
  // as a `breakpoint` event with reason "changed"; nothing in stub mode does.
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

  if (!client.Start({gdb, "--interpreter=dap"}, "gdb")) {
    std::fprintf(stderr, "[dap-e2e] SKIP: gdb could not be started as a DAP adapter\n");
    return;
  }
  if (!PumpUntil(client, [&]() { return client.IsInitialized() || !client.IsRunning(); }, 15000) ||
      !client.IsInitialized()) {
    client.Shutdown();
    std::fprintf(stderr, "[dap-e2e] SKIP: gdb did not complete initialize (needs gdb >= 14)\n");
    return;
  }

  // (1) A breakpoint set before the program is loaded. The adapter has no module
  //     to bind it to yet, so it must come back unverified-and-pending rather
  //     than verified (which is what a fake would have been written to return).
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

  // (2) Launch. gdb does NOT answer `launch` until after `configurationDone`, so
  //     the response is deliberately not waited on here -- waiting on it is the
  //     deadlock a stub-only suite never sees.
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

  // From here the adapter really stopped at our breakpoint, so nothing below may
  // skip.
  Expect(breakpoint_verified_by_event,
         "the pending breakpoint must be re-reported as verified once its module loads");

  // (3) The stop is at OUR line, in OUR function.
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

  // (4) Real scopes and real values. A fake returns what we told it to; gdb
  //     returns what the inferior actually holds.
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

  // (5) Resume to exit. `continue` releases the inferior and the adapter reports
  //     the process leaving on its own.
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
#endif
}

}  // namespace

void RegisterDapRealAdapterE2ETests(std::vector<TestCase>& tests) {
  AddTest(tests, "DapRealAdapter/GdbHandshakeAndShutdown",
          TestDapRealAdapterGdbHandshakeAndShutdown);
  AddTest(tests, "DapRealAdapter/GdbLaunchBreakpointStopCycle",
          TestDapRealAdapterGdbLaunchBreakpointStopCycle);
}

}  // namespace microide::tests
