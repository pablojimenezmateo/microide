#include "workspace/debug/DebugService.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "editor/EditTypes.h"
#include "util/DebugTrace.h"
#include "workspace/WorkspaceContext.h"
#include "workspace/state/WorkspaceProjectState.h"

namespace microide::workspace {

void DebugService::ToggleExceptionFilter(const std::string& filter_id) {
  DebugBreakpointsModel& panel = CurrentProjectState().debug_breakpoints_panel;
  if (!panel.ToggleFilter(filter_id)) {
    return;
  }
  ResendExceptionFiltersAndSync();
}

std::string DebugService::SetExceptionFilterCondition(const std::string& filter_id,
                                                      std::optional<std::string> condition) {
  DebugBreakpointsModel& panel = CurrentProjectState().debug_breakpoints_panel;
  const auto& advertised = panel.AdvertisedFilters();
  const auto filter = std::find_if(advertised.begin(), advertised.end(),
                                   [&](const dap_protocol::DapExceptionFilter& f) { return f.filter == filter_id; });
  if (filter == advertised.end()) {
    std::string known;
    for (const dap_protocol::DapExceptionFilter& f : advertised) {
      known += known.empty() ? f.filter : ", " + f.filter;
    }
    return "no exception filter \"" + filter_id + "\"" +
           (known.empty() ? std::string(" (no session has advertised any yet)") : " (known: " + known + ")");
  }
  if (condition.has_value() && !condition->empty() && !filter->supports_condition) {
    return "the exception filter \"" + filter_id + "\" takes no condition";
  }
  if (panel.SetFilterCondition(filter_id, std::move(condition))) {
    ResendExceptionFiltersAndSync();
  }
  return {};
}

void DebugService::AddFunctionBreakpoint(std::string name) {
  if (!CurrentProjectState().function_breakpoint_store.Add(std::move(name))) {
    return;
  }
  ResendFunctionBreakpointsAndSync();
}

void DebugService::RemoveFunctionBreakpoint(std::size_t index) {
  CurrentProjectState().function_breakpoint_store.Remove(index);
  ResendFunctionBreakpointsAndSync();
}

void DebugService::ToggleFunctionBreakpointEnabled(std::size_t index) {
  if (!CurrentProjectState().function_breakpoint_store.ToggleEnabled(index)) {
    return;
  }
  ResendFunctionBreakpointsAndSync();
}

void DebugService::SetFunctionBreakpointCondition(std::size_t index,
                                                  std::optional<std::string> condition) {
  CurrentProjectState().function_breakpoint_store.SetCondition(index, std::move(condition));
  ResendFunctionBreakpointsAndSync();
}

namespace {

std::optional<std::size_t> FindFunctionBreakpointIndex(
    const editor::FunctionBreakpointStore& store, const std::string& name) {
  const std::vector<editor::FunctionBreakpoint>& all = store.All();
  for (std::size_t i = 0; i < all.size(); ++i) {
    if (all[i].name == name) {
      return i;
    }
  }
  return std::nullopt;
}

}  // namespace

void DebugService::RemoveFunctionBreakpointByName(const std::string& name) {
  if (const auto index =
          FindFunctionBreakpointIndex(CurrentProjectState().function_breakpoint_store, name);
      index.has_value()) {
    RemoveFunctionBreakpoint(*index);
  }
}

void DebugService::ToggleFunctionBreakpointByName(const std::string& name) {
  if (const auto index =
          FindFunctionBreakpointIndex(CurrentProjectState().function_breakpoint_store, name);
      index.has_value()) {
    ToggleFunctionBreakpointEnabled(*index);
  }
}

void DebugService::SetFunctionBreakpointConditionByName(const std::string& name,
                                                        std::optional<std::string> condition) {
  if (const auto index =
          FindFunctionBreakpointIndex(CurrentProjectState().function_breakpoint_store, name);
      index.has_value()) {
    SetFunctionBreakpointCondition(*index, std::move(condition));
  }
}

void DebugService::ResendFunctionBreakpointsAndSync() {
  SyncBreakpointsPanel();
  if (DebugSession* session = CurrentDapManager().ActiveSession();
      session != nullptr && session->IsActive()) {
    session->ResendFunctionBreakpoints();
  }
  if (operations_.request_debug_pane_redraw) {
    operations_.request_debug_pane_redraw();
  }
}

void DebugService::ResendExceptionFiltersAndSync() {
  SyncBreakpointsPanel();
  // Live re-send so the change takes effect immediately on an active session.
  if (DebugSession* session = CurrentDapManager().ActiveSession();
      session != nullptr && session->IsActive()) {
    session->ResendExceptionFilters();
  }
  if (operations_.request_debug_pane_redraw) {
    operations_.request_debug_pane_redraw();
  }
}

bool DebugService::ShiftBreakpointsForAppliedEdit(const std::filesystem::path& path,
                                                  const editor::AppliedEdit& edit) {
  if (path.empty()) {
    return false;
  }
  if (!CurrentProjectState().breakpoint_store.ShiftForAppliedEdit(path, edit)) {
    return false;
  }
  SyncBreakpointsPanel();
  // Live re-send so an active session rebinds the moved lines immediately; no-op
  // when no session is running.
  ResendBreakpointsForFile(path);
  return true;
}

void DebugService::SyncBreakpointsPanel() {
  ProjectWorkspaceState& state = CurrentProjectState();
  state.debug_breakpoints_panel.Rebuild(state.breakpoint_store, state.function_breakpoint_store);
}

}  // namespace microide::workspace
