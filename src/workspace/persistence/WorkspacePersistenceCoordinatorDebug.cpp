#include "workspace/persistence/WorkspacePersistenceCoordinator.h"

#include <filesystem>
#include <string>
#include <vector>

#include "util/JsonValue.h"
#include "util/InvokeIfSet.h"

namespace microide::workspace {

std::filesystem::path PersistenceCoordinator::DebugStatePath() const {
  return CurrentProjectState().root.empty() ? std::filesystem::path{}
                                            : operations_.project_state_directory() / "debug";
}

void PersistenceCoordinator::SaveDebugState() {
  if (operations_.persistence_service == nullptr) {
    return;
  }
  const std::filesystem::path debug_path = DebugStatePath();
  if (debug_path.empty()) {
    return;
  }
  const ProjectWorkspaceState& state = CurrentProjectState();

  PersistedDebugState persisted;
  std::vector<editor::BreakpointStore::FileBreakpointsView> breakpoint_files;
  state.breakpoint_store.FillSortedFileViews(&breakpoint_files);
  for (const auto& file : breakpoint_files) {
    PersistedFileBreakpoints persisted_file;
    persisted_file.path = *file.path;
    for (const editor::Breakpoint& breakpoint : file.breakpoints) {
      persisted_file.breakpoints.push_back(PersistedBreakpoint{
          .line = breakpoint.line,
          .enabled = breakpoint.enabled,
          .condition = breakpoint.condition,
          .hit_condition = breakpoint.hit_condition,
          .log_message = breakpoint.log_message,
      });
    }
    persisted.files.push_back(std::move(persisted_file));
  }
  for (const LaunchConfig& config : state.launch_configs) {
    persisted.launch_configs.push_back(PersistedLaunchConfig{
        .name = config.name,
        .type = config.type,
        .request = config.request,
        .arguments_json =
            config.arguments.IsNull() ? std::string{} : util::SerializeJson(config.arguments),
    });
  }
  for (const editor::FunctionBreakpoint& fn : state.function_breakpoint_store.All()) {
    persisted.function_breakpoints.push_back(PersistedFunctionBreakpoint{
        .name = fn.name,
        .enabled = fn.enabled,
        .condition = fn.condition,
        .hit_condition = fn.hit_condition,
    });
  }
  persisted.selected_launch_config_index = state.selected_launch_config_index;
  persisted.watch_expressions = state.debug_watch.Expressions();
  persisted.enabled_exception_filters = state.debug_breakpoints_panel.EnabledFilterIds();
  persisted.exception_filters_seeded = state.debug_breakpoints_panel.Seeded();
  persisted.exception_filter_conditions = state.debug_breakpoints_panel.FilterConditions();

  if (persisted.files.empty() && persisted.launch_configs.empty() &&
      persisted.watch_expressions.empty() && persisted.enabled_exception_filters.empty() &&
      persisted.function_breakpoints.empty() && persisted.exception_filter_conditions.empty() &&
      !persisted.exception_filters_seeded) {
    operations_.persistence_service->DeleteState(debug_path);
    return;
  }
  operations_.persistence_service->SaveDebugState(debug_path, persisted);
}

void PersistenceCoordinator::RestoreDebugState() {
  if (operations_.persistence_service == nullptr) {
    return;
  }
  const std::filesystem::path debug_path = DebugStatePath();
  if (debug_path.empty()) {
    return;
  }
  PersistedDebugState persisted;
  if (!operations_.persistence_service->LoadDebugState(debug_path, &persisted)) {
    return;
  }

  ProjectWorkspaceState& state = CurrentProjectState();
  std::vector<editor::BreakpointStore::FileBreakpoints> files;
  files.reserve(persisted.files.size());
  for (const PersistedFileBreakpoints& persisted_file : persisted.files) {
    editor::BreakpointStore::FileBreakpoints file;
    file.path = persisted_file.path;
    for (const PersistedBreakpoint& breakpoint : persisted_file.breakpoints) {
      file.breakpoints.push_back(editor::Breakpoint{
          .line = breakpoint.line,
          .enabled = breakpoint.enabled,
          .condition = breakpoint.condition,
          .hit_condition = breakpoint.hit_condition,
          .log_message = breakpoint.log_message,
      });
    }
    files.push_back(std::move(file));
  }
  state.breakpoint_store.ReplaceAll(std::move(files));

  std::vector<editor::FunctionBreakpoint> function_breakpoints;
  function_breakpoints.reserve(persisted.function_breakpoints.size());
  for (const PersistedFunctionBreakpoint& fn : persisted.function_breakpoints) {
    function_breakpoints.push_back(editor::FunctionBreakpoint{
        .name = fn.name,
        .enabled = fn.enabled,
        .condition = fn.condition,
        .hit_condition = fn.hit_condition,
    });
  }
  state.function_breakpoint_store.ReplaceAll(std::move(function_breakpoints));

  state.launch_configs.clear();
  for (const PersistedLaunchConfig& persisted_config : persisted.launch_configs) {
    LaunchConfig config;
    config.name = persisted_config.name;
    config.type = persisted_config.type;
    config.request = persisted_config.request;
    if (!persisted_config.arguments_json.empty()) {
      if (auto parsed = util::ParseJson(persisted_config.arguments_json); parsed.has_value()) {
        config.arguments = std::move(*parsed);
      }
    }
    state.launch_configs.push_back(std::move(config));
  }
  if (state.launch_configs.empty() ||
      persisted.selected_launch_config_index >= state.launch_configs.size()) {
    state.selected_launch_config_index = 0;
  } else {
    state.selected_launch_config_index = persisted.selected_launch_config_index;
  }
  state.debug_watch.SetExpressions(std::move(persisted.watch_expressions));
  state.debug_breakpoints_panel.SetEnabledFilterIds(std::move(persisted.enabled_exception_filters),
                                                    persisted.exception_filters_seeded);
  state.debug_breakpoints_panel.SetFilterConditions(
      std::move(persisted.exception_filter_conditions));
  state.debug_breakpoints_panel.Rebuild(state.breakpoint_store, state.function_breakpoint_store);
}

}
