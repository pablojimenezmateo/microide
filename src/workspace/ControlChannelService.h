#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "platform/ControlSocketServer.h"
#include "util/JsonValue.h"

namespace microide::workspace {

struct WorkspaceContext;

// A registered debug adapter surfaced by the `adapters` query: its type id plus
// the argv used to spawn it. Mirrors DapManager::AdapterInfo but kept local so
// ControlChannelService stays free of DapManager coupling.
struct ControlAdapterInfo {
  std::string type;
  std::vector<std::string> command;
};

// A live control-channel instance discovered from its descriptor file.
struct ControlInstanceDescriptor {
  int pid = 0;
  std::filesystem::path socket;
  std::string project_root;
  std::string project_hash;
  std::string raw_json;  // descriptor line as written (backs `control-list`)
};

// Enumerate every running instance with the control channel enabled, parsed from
// the per-instance descriptor files under $XDG_RUNTIME_DIR/microide/instances.
// Descriptors whose process is gone (crash / SIGKILL) are pruned as a side
// effect. Backs both `control-list` and `control-send` socket discovery.
std::vector<ControlInstanceDescriptor> EnumerateControlInstances();

// Concatenate the descriptor lines of every running instance (one JSON object
// per line). Backs `microide control-list`. Empty when none are found.
std::string ControlListInstancesText();

// Host-owned home for the live control channel. Owns the AF_UNIX line server,
// drains inbound command/query requests on the main thread, dispatches commands
// through the shell command chokepoint (via Operations), answers queries by
// reading WorkspaceContext, and broadcasts debug events to connected clients.
//
// Mirrors DebugService: a narrow Operations seam, a SetWakeEventType hook, and a
// ConsumeControlCallbacks() drain pumped from the control SDL wake event.
class ControlChannelService {
 public:
  struct CommandOutcome {
    bool ok = false;
    std::string feedback;
    std::string error;
  };

  struct Operations {
    // Run a command line through the same path as the in-app command prompt.
    std::function<CommandOutcome(const std::string&)> execute_command_line;
    // Write one JSONL line to the stdout mirror (the `--control` headless
    // stream). Wired to std::cout; left null in tests / non-headless runs.
    std::function<void(const std::string&)> emit_jsonl;
    // Registered debug adapters (type + spawn command) for the `adapters` query.
    // Kept behind a callback so the service stays free of DapManager coupling.
    std::function<std::vector<ControlAdapterInfo>()> adapters;
    // Enable the debugger transiently if it is off. Invoked before a
    // breakpoint-*/debug-* command so the channel never needs a separate
    // `set-setting debug.enabled true` prelude. Left null in tests.
    std::function<void()> ensure_debugger_enabled;
  };

  // A single accepted control query (`debug-state`, `breakpoints`, `tabs`,
  // `projects`, `launch-configs`, ...) is answered on the main thread by building a
  // full JSON array/object before serialization. The socket layer caps request
  // lines, inbound bytes, connections, and write buffers, but nothing caps how much
  // workspace/debug state a single query materializes. Bound every query array at
  // this item budget so a huge stack, breakpoint set, or tab list can't allocate and
  // serialize a multi-megabyte response on the UI path. TD-2026-07-17A-095.
  static constexpr std::size_t kMaxControlQueryEntries = 10000;

  ControlChannelService() = default;
  ~ControlChannelService();
  ControlChannelService(const ControlChannelService&) = delete;
  ControlChannelService& operator=(const ControlChannelService&) = delete;

  void Configure(WorkspaceContext& context, Operations operations);
  void SetWakeEventType(std::uint32_t event_type);

  // Mirror responses/events to stdout as JSONL (the `--control` stream). When on,
  // debug events surface even with zero socket clients.
  void SetStdoutMirror(bool on) { stdout_mirror_ = on; }
  bool StdoutMirrorEnabled() const { return stdout_mirror_; }

  // Emit one already-serialized JSONL line to the stdout mirror (no-op when the
  // mirror is off or no sink is wired). Used for cold-start `applied` lines.
  void EmitJsonLine(const std::string& line) const;

  // Start/stop the listener for `project_root`. Start binds the socket and
  // writes the per-instance descriptor; Stop removes the descriptor. Idempotent.
  // On a fresh bind a `{"event":"ready",...}` line is mirrored to stdout.
  bool Start(const std::filesystem::path& project_root);
  void Stop();
  bool IsRunning() const;

  // Main thread: drain + dispatch queued inbound requests (control wake event).
  void ConsumeControlCallbacks();

  // Main thread: push debug notifications to connected clients. No-ops when the
  // server is not running.
  //
  // A stop is reported in two phases: `OnDebugStopBegan` fires the instant the
  // adapter halts, carrying the real reason/thread with `framesPending:true`
  // (file/line/frames omitted); `OnDebugStopped` fires once the stack resolves,
  // carrying the populated frames with `framesPending:false`. Agents disambiguate
  // on the `framesPending` flag.
  void OnDebugStopBegan(const std::string& reason, int thread_id);
  void OnDebugStopped();
  // `reason` carries the teardown message on a non-clean end (crash / kill /
  // launch rejection); it is emitted as a `reason` field when non-empty and omitted
  // for a clean exit.
  void OnDebugTerminated(int session_id, const std::string& reason = {});
  void OnDebugOutput(const std::string& category, const std::string& text);

  std::size_t ConnectionCount() const;

 private:
  util::JsonValue HandleQuery(const std::string& verb, const util::JsonValue& args, bool* ok,
                              std::string* error) const;
  util::JsonValue BuildDebugState() const;
  util::JsonValue BuildBreakpoints() const;
  util::JsonValue BuildFunctionBreakpoints() const;
  util::JsonValue BuildExceptionFilters() const;
  util::JsonValue BuildTabs() const;
  util::JsonValue BuildProjects() const;
  util::JsonValue BuildStatus() const;
  util::JsonValue BuildLaunchConfigs() const;
  util::JsonValue BuildAdapters() const;

  // Broadcast an event to connected clients (when running) and mirror it to
  // stdout (when mirroring). Surfaces events even with no socket client.
  void EmitEvent(util::JsonValue event);

  // (Re)write the per-instance discovery descriptor (`instances/<pid>.json`).
  // Called at Start and again when the socket server re-binds after the socket
  // (and descriptor) vanished mid-run. Main thread only.
  void WriteDescriptor();

  WorkspaceContext* context_ = nullptr;
  Operations operations_{};
  platform::ControlSocketServer server_;
  std::uint32_t wake_event_type_ = 0;
  std::filesystem::path descriptor_path_;
  std::filesystem::path project_root_;
  bool stdout_mirror_ = false;
};

}  // namespace microide::workspace
