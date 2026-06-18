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
  };

  ControlChannelService() = default;
  ~ControlChannelService();
  ControlChannelService(const ControlChannelService&) = delete;
  ControlChannelService& operator=(const ControlChannelService&) = delete;

  void Configure(WorkspaceContext& context, Operations operations);
  void SetWakeEventType(std::uint32_t event_type);

  // Start/stop the listener for `project_root`. Start binds the socket and
  // writes the per-instance descriptor; Stop removes the descriptor. Idempotent.
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

  WorkspaceContext* context_ = nullptr;
  Operations operations_{};
  platform::ControlSocketServer server_;
  std::uint32_t wake_event_type_ = 0;
  std::filesystem::path descriptor_path_;
};

}  // namespace microide::workspace
