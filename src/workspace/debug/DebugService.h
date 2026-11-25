#pragma once

#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "util/Generation.h"
#include "workspace/debug/DapProtocol.h"
#include "workspace/debug/DebugSession.h"
#include "workspace/debug/LaunchConfig.h"
#include "workspace/debug/WorkspaceDapManager.h"

namespace microide::editor {
struct AppliedEdit;
class TextViewport;
}

namespace microide::workspace {

class DebugValueTreePane;

struct WorkspaceContext;
struct ProjectWorkspaceState;

class DebugService {
 public:
  struct Operations {
    std::function<void(int session_id, const std::string& label,
                       const dap_protocol::DapOutputEvent& output)>
        append_console_output;
    std::function<void(int session_id, const std::string& label)> show_debug_console;
    std::function<void(int session_id)> remove_debug_console;
    std::function<void(DebugSession::State state)> notify_session_state_changed;
    std::function<void(int session_id, bool failed, const std::string& reason)>
        notify_session_terminated;
    std::function<void(const std::string& reason, int thread_id)> notify_stop_began;
    std::function<void()> notify_stop_resolved;
    std::function<void()> request_frame;
    std::function<void()> queue_editor_hover_refresh;
    std::function<void(const std::filesystem::path& path, std::size_t line)> focus_source_location;
    std::function<void()> show_call_stack_panel;
  };

  DebugService() = default;

  void Configure(WorkspaceContext& context, Operations operations);
  void SetWakeChannel(Uint32 event_type);

  DapManager& CurrentDapManager();
  const DapManager& CurrentDapManager() const;
  DapManager& EnsureProjectDapManager(ProjectWorkspaceState& state);
  void ConsumeDapCallbacks();

  bool StartDebugging(const LaunchConfig& config, const std::string& cwd = {});
  void StopDebugging();
  void StopAllDebugging();

  void ResendBreakpointsForFile(const std::filesystem::path& path);

  bool ShiftBreakpointsForAppliedEdit(const std::filesystem::path& path,
                                      const editor::AppliedEdit& edit);
  bool ShiftBreakpointsForViewportEdit(const editor::TextViewport& viewport);

  void Continue();
  void StepOver();
  void StepIn();
  void StepOut();
  void Pause();
  void ReverseContinue();
  void StepBack();
  void Restart();

  void FocusThread(int thread_id);

  void FocusSession(int session_id);
  void FocusNextSession();
  std::vector<DapSessionInfo> Sessions() const;
  std::size_t SessionCount() const;
  void SwitchSession(int index);

  void ToggleExceptionFilter(const std::string& filter_id);
  std::string SetExceptionFilterCondition(const std::string& filter_id,
                                          std::optional<std::string> condition);

  void AddFunctionBreakpoint(std::string name);
  void RemoveFunctionBreakpoint(std::size_t index);
  void ToggleFunctionBreakpointEnabled(std::size_t index);
  void SetFunctionBreakpointCondition(std::size_t index, std::optional<std::string> condition);
  void RemoveFunctionBreakpointByName(const std::string& name);
  void ToggleFunctionBreakpointByName(const std::string& name);
  void SetFunctionBreakpointConditionByName(const std::string& name,
                                            std::optional<std::string> condition);

  void SyncBreakpointsPanel();

  void FocusFrame(int frame_id);
  void ToggleVariableRow(std::size_t row);
  void BeginVariableEdit(std::size_t row) { BeginTreeEdit(EditableTree::Variables, row); }
  void CommitVariableEdit() { CommitTreeEdit(EditableTree::Variables); }
  void CancelVariableEdit() { CancelTreeEdit(EditableTree::Variables); }

  void EvaluateWatches(int frame_id);
  std::size_t AddWatch(std::string expression);
  void EditWatch(std::size_t index, std::string expression);
  void RemoveWatch(std::size_t index);
  void ToggleWatchRow(std::size_t row);
  void BeginWatchEdit(std::size_t row) { BeginTreeEdit(EditableTree::Watch, row); }
  void CommitWatchEdit() { CommitTreeEdit(EditableTree::Watch); }
  void CancelWatchEdit() { CancelTreeEdit(EditableTree::Watch); }
  int FocusedFrameId() const;

  void EvaluateHover(int frame_id, const std::string& expression);
  bool SupportsStepBack() const;

  bool EvaluateRepl(const std::string& expression);

  bool IsSessionActive() const;
  int ActiveSessionId() const;
  std::string ActiveSessionLabel() const;
  DebugSession::State SessionState() const;
  std::string LastError() const;

  bool HasInFlightDapWork() const;

 private:
  enum class EditableTree { Variables, Watch };
  DebugValueTreePane& EditablePane(EditableTree tree);
  util::Generation& EditableGeneration(EditableTree tree);
  void BeginTreeEdit(EditableTree tree, std::size_t row);
  void CommitTreeEdit(EditableTree tree);
  void CancelTreeEdit(EditableTree tree);
  ProjectWorkspaceState& CurrentProjectState();
  const ProjectWorkspaceState& CurrentProjectState() const;
  DebugSession::Callbacks BuildSessionCallbacks(int session_id, std::string session_label);
  bool IsActiveSession(int session_id) const;
  int LaunchSession(const LaunchConfig& config, const std::string& cwd, bool replace);
  template <typename Model>
  void FetchTreeChildrenPage(Model ProjectWorkspaceState::*model_member,
                             util::Generation& generation,
                             int reference,
                             int start,
                             int count);
  void FetchVariablesPage(int reference, int start, int count);
  void FetchWatchChildren(int reference, int start, int count);
  void ProjectStop(const dap_protocol::DapStoppedEvent& stop,
                   const std::vector<dap_protocol::DapStackFrame>& frames);
  void SyncSessionsPanel();
  void ResendFunctionBreakpointsAndSync();
  void ResendExceptionFiltersAndSync();
  void ClearTransientDebugViews();
  void ResetAdapterBreakpointState();
  void AppendConsoleLine(int session_id, const std::string& label, const std::string& text);

  WorkspaceContext* context_ = nullptr;
  Operations operations_{};
  Uint32 wake_event_type_ = 0;
  LaunchConfig last_launch_config_{};
  std::string last_cwd_;
  std::vector<int> ended_console_ids_;
  std::unordered_map<std::string, std::uint64_t> breakpoints_followed_revision_;
  util::Generation frame_generation_;
  util::Generation watch_generation_;
};

}
