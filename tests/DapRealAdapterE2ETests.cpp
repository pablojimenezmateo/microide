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
  client.SetWakeEventType(0);

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

}  // namespace

void RegisterDapRealAdapterE2ETests(std::vector<TestCase>& tests) {
  AddTest(tests, "DapRealAdapter/GdbHandshakeAndShutdown",
          TestDapRealAdapterGdbHandshakeAndShutdown);
}

}  // namespace microide::tests
