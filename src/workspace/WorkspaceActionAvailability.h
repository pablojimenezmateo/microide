#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "editor/TextViewport.h"
#include "workspace/WorkspaceActionTypes.h"
#include "workspace/WorkspaceContext.h"
#include "workspace/WorkspaceProjectState.h"
#include "workspace/WorkspaceTextInputState.h"

namespace microide::workspace {

class ActionAvailability {
 public:
  struct Operations {
    std::function<TreeContextTargetKind()> selected_tree_target_kind;
    std::function<std::filesystem::path(ActionSource)> resolve_tree_action_path;
    std::function<std::filesystem::path()> active_tab_path;
    std::function<const editor::TextViewport*()> active_navigable_viewport;
    std::function<const editor::TextViewport*()> active_editable_viewport;
    std::function<TextInputSurface()> current_text_input_surface;
    std::function<bool()> active_single_line_text_has_selection;
    std::function<const TerminalTabState*()> active_terminal_tab;
    // Cheap "is Copy Last Command available?" predicate. Enablement must NOT build the
    // whole scrollback transcript (that is LastTerminalCommandText's job on invoke).
    std::function<bool()> has_last_terminal_command;
    std::function<bool()> terminal_has_selection;
    std::function<bool()> active_tab_is_editor;
    std::function<std::size_t()> editor_group_count;
    std::function<bool()> active_tab_is_compare;
    std::function<bool()> active_tab_is_merge;
    std::function<const CompareTabState*()> active_compare_tab;
    std::function<bool()> active_completion_available;
    std::function<bool()> active_code_actions_available;
    std::function<bool()> active_definition_available;
    std::function<bool()> active_references_available;
    std::function<std::optional<std::string>(std::string_view)> get_setting_value;
    // Debugger (DAP) execution-control gating: active = a session is running or
    // paused; stopped = that session is paused at a breakpoint/step.
    std::function<bool()> debug_session_active;
    std::function<bool()> debug_session_stopped;
    // Whether the active session's adapter advertises `supportsStepBack` — gates
    // the reverse-execution commands so they only light up for recording adapters.
    std::function<bool()> debug_supports_reverse;
    // Number of live debug sessions (Phase 8) — gates the session switcher.
    std::function<std::size_t()> debug_session_count;
  };

  ActionAvailability(const WorkspaceContext& context, Operations operations);

  bool IsEnabled(ActionId id) const;

 private:
  TreeContextTargetKind ActiveTreeTargetKind() const;

  const WorkspaceContext& context_;
  Operations operations_;
};

}  // namespace microide::workspace
