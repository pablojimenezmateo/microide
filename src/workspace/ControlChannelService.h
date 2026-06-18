#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

#include "platform/ControlSocketServer.h"
#include "util/JsonValue.h"

namespace microide::workspace {

struct WorkspaceContext;

// Concatenate the descriptor files of every running instance that has the
// control channel enabled (one JSON object per line). Backs `microide
// control-list`. Returns an empty string when none are found.
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
    // Registered debug-adapter type ids (for the `adapters` query). Kept behind a
    // callback so the service stays free of DapManager/DebugService coupling.
    std::function<std::vector<std::string>()> adapter_types;
  };

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
  void OnDebugStopped();
  void OnDebugTerminated(int session_id);
  void OnDebugOutput(const std::string& category, const std::string& text);

  std::size_t ConnectionCount() const;

 private:
  util::JsonValue HandleQuery(const std::string& verb, const util::JsonValue& args, bool* ok,
                              std::string* error) const;
  util::JsonValue BuildDebugState() const;
  util::JsonValue BuildBreakpoints() const;
  util::JsonValue BuildTabs() const;
  util::JsonValue BuildProjects() const;
  util::JsonValue BuildStatus() const;
  util::JsonValue BuildLaunchConfigs() const;
  util::JsonValue BuildAdapters() const;

  // Broadcast an event to connected clients (when running) and mirror it to
  // stdout (when mirroring). Surfaces events even with no socket client.
  void EmitEvent(util::JsonValue event);

  WorkspaceContext* context_ = nullptr;
  Operations operations_{};
  platform::ControlSocketServer server_;
  std::uint32_t wake_event_type_ = 0;
  std::filesystem::path descriptor_path_;
  bool stdout_mirror_ = false;
};

}  // namespace microide::workspace
