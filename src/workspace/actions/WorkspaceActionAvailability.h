#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "editor/TextViewport.h"
#include "workspace/actions/WorkspaceActionTypes.h"
#include "workspace/WorkspaceContext.h"
#include "workspace/state/WorkspaceProjectState.h"
#include "workspace/state/WorkspaceTextInputState.h"

namespace microide::workspace {

class DebugService;
class RemoteHostService;

class ActionAvailability {
 public:
  struct Operations {
    std::function<TreeContextTargetKind()> selected_tree_target_kind;
    std::function<std::filesystem::path(ActionSource)> resolve_tree_action_path;
    // Path the currently open row context menu targets, empty when none is open.
    // See the matching field on WorkspaceActionContext::Operations.
    std::function<std::filesystem::path()> row_context_menu_path;
    std::function<std::filesystem::path()> active_tab_path;
    std::function<const editor::TextViewport*()> active_navigable_viewport;
    std::function<const editor::TextViewport*()> active_editable_viewport;
    std::function<TextInputSurface()> current_text_input_surface;
    std::function<bool()> active_single_line_text_has_selection;
    // Cheap "is Copy Last Command available?" predicate. Enablement must NOT build the
    // whole scrollback transcript (that is LastTerminalCommandText's job on invoke).
    std::function<bool()> has_last_terminal_command;
    std::function<bool()> terminal_has_selection;
    std::function<bool()> active_tab_is_editor;
    std::function<std::size_t()> editor_group_count;
    std::function<bool()> active_tab_is_compare;
    std::function<bool()> active_tab_is_merge;
    std::function<const CompareTabState*()> active_compare_tab;
    std::function<const MergeTabState*()> active_merge_tab;
    std::function<bool()> active_completion_available;
    std::function<bool()> active_code_actions_available;
    std::function<bool()> active_definition_available;
    std::function<bool()> active_references_available;
    std::function<std::optional<std::string>(std::string_view)> get_setting_value;
    // The remote hosts service when one exists (null before any remote project
    // opened: nothing is connected), and whether the recents list names a remote
    // project whose mirror is on disk -- what Remove Mirror can take while no
    // service exists yet (cached against the recents revision).
    std::function<const RemoteHostService*()> remote_hosts;
    std::function<bool()> has_remote_recent;
  };

  // `debug` answers the execution-control gating directly (a session running or
  // paused, a recording adapter, more than one session).
  ActionAvailability(const WorkspaceContext& context, const DebugService& debug,
                     Operations operations);

  bool IsEnabled(ActionId id) const;
  // Why `id` is dimmed, for the menu row to say after its label ("Show Status
  // (connect to a host first)"), as the LSP rows say their server's state; empty
  // when it is enabled or has no reason worth a word. Static text, no allocation:
  // asked per painted row.
  std::string_view DisabledReason(ActionId id) const;
  // The reason `id` would give when dimmed, whatever its state now (empty for an
  // action that never gives one): what a popup's width is measured with, so the
  // menu does not change width as a host connects.
  static std::string_view PossibleDisabledReason(ActionId id);

 private:
  TreeContextTargetKind ActiveTreeTargetKind() const;

  const WorkspaceContext& context_;
  const DebugService& debug_;
  Operations operations_;
};

}  // namespace microide::workspace
