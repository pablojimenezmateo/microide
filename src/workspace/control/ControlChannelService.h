#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "platform/ControlSocketServer.h"
#include "util/JsonValue.h"
#include "workspace/control/ControlBufferQuery.h"
#include "workspace/control/ControlUiQuery.h"
#include "workspace/registries/WorkspaceKeybindingRegistry.h"
#include "workspace/registries/WorkspaceSettingsRegistry.h"
#include "workspace/services/NotificationService.h"

namespace microide::workspace {

class WorkspaceOutputChannels;

struct WorkspaceContext;

struct ControlAdapterInfo {
  std::string type;
  std::vector<std::string> command;
};

struct ControlRemoteProjectInfo {
  std::string host;
  std::string host_root;
  std::string state;
  std::int64_t files = 0;
  std::int64_t files_pending = 0;
  std::int64_t unsent_pushes = 0;
  std::int64_t conflicts = 0;
  std::int64_t blocked = 0;
  std::int64_t held_deletes = 0;
};

struct ControlInstanceDescriptor {
  int pid = 0;
  std::filesystem::path socket;
  std::string project_root;
  std::string project_hash;
  std::string raw_json;
};

std::vector<ControlInstanceDescriptor> EnumerateControlInstances();

std::string ControlListInstancesText();

struct ControlPersistenceState {
  bool crash_flush_armed = false;
  bool writes_settled = true;
};

class ControlChannelService {
 public:
  struct CommandOutcome {
    bool ok = false;
    std::string feedback;
    std::string error;
  };

  struct Operations {
    std::function<CommandOutcome(const std::string&)> execute_command_line;
    std::function<void(const std::string&)> emit_jsonl;
    std::function<std::vector<ControlAdapterInfo>()> adapters;
    std::function<void()> ensure_debugger_enabled;
    const NotificationService* notifications = nullptr;
    std::function<std::optional<ControlRemoteProjectInfo>(const std::filesystem::path& root)>
        remote_project;
    std::function<const std::vector<ResolvedKeybinding>&()> keybindings;
    std::function<bool(ActionId)> action_enabled;
    ControlUiQuery::Operations ui;
    std::function<std::string_view(float, float)> window_hit_region;
    std::function<ControlActiveBuffer()> active_buffer;
    const WorkspaceOutputChannels* output_channels = nullptr;
    std::function<std::vector<std::string>()> plugin_command_names;
    std::function<std::vector<SettingInfo>()> settings;
    std::function<std::optional<std::string>(std::string_view id)> setting_value;
    std::function<ControlPersistenceState()> persistence;
  };

  static constexpr std::size_t kMaxControlQueryEntries = 10000;

  ControlChannelService() = default;
  ~ControlChannelService();
  ControlChannelService(const ControlChannelService&) = delete;
  ControlChannelService& operator=(const ControlChannelService&) = delete;

  void Configure(WorkspaceContext& context, Operations operations);
  void SetWakeChannel(std::uint32_t event_type);

  void SetStdoutMirror(bool on) { stdout_mirror_ = on; }

  void EmitJsonLine(const std::string& line) const;

  [[nodiscard]] bool Start(const std::filesystem::path& project_root);
  void Stop();
  bool IsRunning() const;

  void ConsumeControlCallbacks();
  void ReleaseIdleWaiters();

  void OnDebugStopBegan(const std::string& reason, int thread_id);
  void OnDebugStopped();
  void OnDebugTerminated(int session_id, const std::string& reason = {});
  void OnDebugOutput(const std::string& category, const std::string& text);

  util::JsonValue AnswerQuery(const std::string& verb, bool* ok, std::string* error) const {
    return HandleQuery(verb, util::JsonValue(util::JsonObject{}), ok, error);
  }
  util::JsonValue AnswerQuery(const std::string& verb, const util::JsonValue& args, bool* ok,
                              std::string* error) const {
    return HandleQuery(verb, args, ok, error);
  }

 private:
  util::JsonValue HandleQuery(const std::string& verb, const util::JsonValue& args, bool* ok,
                              std::string* error) const;
  util::JsonValue BuildDebugState() const;
  util::JsonValue BuildBreakpoints() const;
  util::JsonValue BuildFunctionBreakpoints() const;
  util::JsonValue BuildExceptionFilters() const;
  util::JsonValue BuildTabs() const;
  util::JsonValue BuildNotifications(bool recent) const;
  util::JsonValue BuildOutput(const util::JsonValue& args) const;
  util::JsonValue BuildKeybindings() const;
  util::JsonValue BuildSettings() const;
  util::JsonValue BuildEditor() const;
  util::JsonValue BuildDocuments(const util::JsonValue& args) const;
  util::JsonValue BuildSurface() const;
  util::JsonValue BuildCommands() const;
  util::JsonValue BuildTerminals() const;
  util::JsonValue BuildTerminalOutput(const util::JsonValue& args, bool* ok,
                                      std::string* error) const;
  util::JsonValue BuildProjects() const;
  util::JsonValue BuildStatus() const;
  util::JsonValue BuildLaunchConfigs() const;
  util::JsonValue BuildAdapters() const;

  void EmitEvent(util::JsonValue event);

  void WriteDescriptor();

  WorkspaceContext* context_ = nullptr;
  Operations operations_{};
  platform::ControlSocketServer server_;
  std::uint32_t wake_event_type_ = 0;
  std::filesystem::path descriptor_path_;
  std::filesystem::path project_root_;
  bool stdout_mirror_ = false;
  std::vector<std::pair<std::uint64_t, std::optional<std::int64_t>>> idle_waiters_;
};

}
