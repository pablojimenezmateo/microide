#include "workspace/WorkspaceCommandPromptCoordinator.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "util/SingleLineText.h"
#include "workspace/WorkspaceActionCoordinator.h"
#include "workspace/WorkspaceCommandParsing.h"
#include "workspace/WorkspaceConstants.h"
#include "workspace/WorkspaceCommandRegistry.h"
#include "workspace/WorkspacePathUtils.h"
#include "workspace/WorkspaceSidebarRegistry.h"
#include "workspace/WorkspaceTextSearch.h"
#include "workspace/WorkspaceUiText.h"

namespace microide::workspace {

CommandPromptCoordinator::CommandPromptCoordinator(
    ProjectWorkspaceState& state,
    std::vector<std::string>& available_colorscheme_names,
    Operations operations)
    : state_(state),
      available_colorscheme_names_(available_colorscheme_names),
      operations_(std::move(operations)) {}

void CommandPromptCoordinator::ResetSessionState() {
  state_.panel.command.history_index.reset();
  state_.panel.command.history_pending_input.clear();
  ClearFeedback();
}

void CommandPromptCoordinator::ClearFeedback() {
  state_.panel.command.feedback_text.clear();
}

void CommandPromptCoordinator::SetFeedback(std::string feedback) {
  state_.panel.command.feedback_text = std::move(feedback);
}

bool CommandPromptCoordinator::RejectAction(ActionSource source, std::string feedback) {
  if (source != ActionSource::Command) {
    return true;
  }
  SetFeedback(std::move(feedback));
  return false;
}

void CommandPromptCoordinator::AppendInput(std::string_view input) {
  state_.panel.command.input.text.append(input);
  state_.panel.command.input.cursor = state_.panel.command.input.text.size();
  state_.panel.command.input.selection_anchor.reset();
  state_.panel.command.history_index.reset();
  state_.panel.command.history_pending_input.clear();
  ClearFeedback();
}

void CommandPromptCoordinator::PushHistory(std::string command_line) {
  if (command_line.empty()) {
    return;
  }
  if (!state_.panel.command.history.empty() &&
      state_.panel.command.history.back() == command_line) {
    return;
  }

  state_.panel.command.history.push_back(std::move(command_line));
  if (state_.panel.command.history.size() > 64) {
    state_.panel.command.history.erase(state_.panel.command.history.begin());
  }
}

void CommandPromptCoordinator::StepHistory(int delta) {
  if (delta == 0 || state_.panel.command.history.empty()) {
    return;
  }

  if (!state_.panel.command.history_index.has_value()) {
    if (delta > 0) {
      return;
    }
    state_.panel.command.history_pending_input = state_.panel.command.input.text;
    state_.panel.command.history_index = state_.panel.command.history.size() - 1;
  } else if (delta < 0) {
    if (*state_.panel.command.history_index > 0) {
      --(*state_.panel.command.history_index);
    }
  } else if (*state_.panel.command.history_index + 1 < state_.panel.command.history.size()) {
    ++(*state_.panel.command.history_index);
  } else {
    state_.panel.command.history_index.reset();
    util::SetSingleLineText(&state_.panel.command.input,
                            state_.panel.command.history_pending_input);
    state_.panel.command.history_pending_input.clear();
    ClearFeedback();
    return;
  }

  util::SetSingleLineText(&state_.panel.command.input,
                          state_.panel.command.history[*state_.panel.command.history_index]);
  ClearFeedback();
}

void CommandPromptCoordinator::CompleteInput() {
  const ParsedCommandLine parsed = ParseCommandLine(state_.panel.command.input.text);
  if (parsed.dangling_escape) {
    SetFeedback("Command completion stopped at a trailing escape");
    return;
  }

  const bool starts_new_token = parsed.open_quote == '\0' && parsed.trailing_space;
  const std::size_t active_index =
      starts_new_token ? parsed.tokens.size()
                       : (parsed.tokens.empty() ? 0 : parsed.tokens.size() - 1);
  const std::string command =
      parsed.tokens.empty() ? std::string{} : parsed.tokens.front().text;
  const std::string active_prefix =
      starts_new_token || parsed.tokens.empty() ? std::string{} : parsed.tokens.back().text;
  const std::size_t replace_start = starts_new_token || parsed.tokens.empty()
                                        ? state_.panel.command.input.text.size()
                                        : parsed.tokens.back().start;
  const std::filesystem::path completion_root =
      state_.root.empty() ? std::filesystem::current_path() : state_.root;
  std::vector<std::string> command_names = WorkspaceCommandNames();
  const auto plugin_command_names = operations_.plugin_command_names();
  command_names.insert(command_names.end(), plugin_command_names.begin(), plugin_command_names.end());

  std::vector<CommandCompletionCandidate> candidates;
  if (active_index == 0) {
    candidates = CompleteFromValues(active_prefix, command_names);
  } else if (command == "colorscheme" && active_index == 1) {
    candidates = CompleteFromValues(active_prefix, available_colorscheme_names_);
    if (StartsWith("list", active_prefix)) {
      candidates.push_back(CommandCompletionCandidate{"list", true});
    }
  } else if (command == "focus") {
    candidates = CompleteFromList(active_prefix, kFocusTargetNames);
  } else if (command == "project-open") {
    candidates = CompletePath(completion_root, active_prefix, true);
  } else if (command == "open" || command == "tab" || command == "compare" ||
             command == "term" || command == "vsplit") {
    candidates = CompletePath(completion_root, active_prefix, false);
  } else if (command == "tabswitch") {
    const std::string lowered_prefix = ToLower(active_prefix);
    std::vector<std::string> seen_values;
    auto add_candidate = [&](std::string value) {
      if (value.empty()) {
        return;
      }
      const std::string lowered_value = ToLower(value);
      if (!StartsWith(lowered_value, lowered_prefix)) {
        return;
      }
      if (std::find(seen_values.begin(), seen_values.end(), value) != seen_values.end()) {
        return;
      }
      seen_values.push_back(value);
      candidates.push_back(CommandCompletionCandidate{std::move(value), true});
    };

    for (std::size_t i = 0; i < state_.open_tabs.size(); ++i) {
      add_candidate(std::to_string(i + 1));
      add_candidate(state_.open_tabs[i].title);
      add_candidate(RelativePathLabel(state_.root, state_.open_tabs[i].path));
    }
  } else if (command == "tree" || command == "files") {
    candidates = CompletePath(completion_root, active_prefix, true);
  } else if (command == "sidebar-show" || command == "sidebar-toggle") {
    if (active_index == 1) {
      std::vector<std::string> sidebar_names = operations_.sidebar_view_ids();
      candidates = CompleteFromValues(active_prefix, sidebar_names);
    } else if (parsed.tokens.size() >= 2 && parsed.tokens[1].text == "tree" &&
               active_index == 2) {
      candidates = CompletePath(completion_root, active_prefix, true);
    }
  } else if (command == "soft-tabs" && active_index == 1) {
    candidates = CompleteFromList(active_prefix, kToggleValues);
  } else if (command == "ui-scale" && active_index == 1) {
    candidates = CompleteFromList(active_prefix, kUiScaleCommands);
  }

  if (candidates.empty()) {
    SetFeedback("No completion matches");
    return;
  }

  const std::string common_prefix = CommonPrefix(candidates);
  const bool can_extend_prefix = common_prefix.size() > active_prefix.size();
  if (candidates.size() == 1 || can_extend_prefix) {
    CommandCompletionCandidate candidate =
        candidates.size() == 1 ? candidates.front()
                               : CommandCompletionCandidate{common_prefix, false};
    const std::string replacement = FormatCommandCompletionToken(candidate);
    state_.panel.command.input.text.erase(replace_start);
    state_.panel.command.input.text += replacement;
    state_.panel.command.input.cursor = state_.panel.command.input.text.size();
    state_.panel.command.input.selection_anchor.reset();
  }

  if (candidates.size() == 1) {
    SetFeedback("Completed " + candidates.front().value);
    return;
  }

  std::string matches = "Matches:";
  const std::size_t visible_count = std::min<std::size_t>(6, candidates.size());
  for (std::size_t i = 0; i < visible_count; ++i) {
    matches += (i == 0 ? " " : "  ");
    matches += candidates[i].value;
  }
  if (candidates.size() > visible_count) {
    matches += "  ...";
  }
  SetFeedback(std::move(matches));
}

bool CommandPromptCoordinator::HandleKeyDown(const SDL_KeyboardEvent& event) {
  switch (event.key) {
    case SDLK_ESCAPE: {
      const bool bottom_panel_was_visible = operations_.bottom_panel_visible();
      state_.panel.command_mode = false;
      util::SetSingleLineText(&state_.panel.command.input, "");
      ResetSessionState();
      operations_.request_command_mode_transition_redraw(bottom_panel_was_visible);
      return true;
    }
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
      if (state_.panel.command.input.text.empty() ||
          ExecuteCommandLine(state_.panel.command.input.text)) {
        const bool bottom_panel_was_visible = operations_.bottom_panel_visible();
        state_.panel.command_mode = false;
        util::SetSingleLineText(&state_.panel.command.input, "");
        ResetSessionState();
        operations_.request_command_mode_transition_redraw(bottom_panel_was_visible);
      }
      return true;
    case SDLK_UP:
      StepHistory(-1);
      return true;
    case SDLK_DOWN:
      StepHistory(1);
      return true;
    case SDLK_TAB:
      CompleteInput();
      return true;
    default:
      return false;
  }
}

std::string CommandPromptCoordinator::PromptStatusText(const CommandState& command) {
  if (!command.feedback_text.empty()) {
    return command.feedback_text;
  }
  if (command.history_index.has_value()) {
    return "History " + std::to_string(*command.history_index + 1) + " / " +
           std::to_string(command.history.size()) + "  |  " +
           JoinHintSegments({"Enter run", "Esc cancel", "Tab complete"});
  }
  return JoinHintSegments({"Enter run", "Esc cancel", "Up/Down history", "Tab complete"});
}

bool CommandPromptCoordinator::ExecuteCommandLine(const std::string& command_line) {
  const ParsedCommandLine parsed = ParseCommandLine(command_line);
  if (parsed.dangling_escape) {
    SetFeedback("Command parse error: trailing escape");
    return false;
  }
  if (parsed.open_quote != '\0') {
    SetFeedback(std::string("Command parse error: unterminated ") +
                (parsed.open_quote == '\'' ? "single" : "double") + " quote");
    return false;
  }
  if (parsed.tokens.empty()) {
    return true;
  }

  PushHistory(command_line);
  ClearFeedback();
  const std::string& command = parsed.tokens.front().text;
  std::vector<std::string> args;
  args.reserve(parsed.tokens.size() - 1);
  for (std::size_t i = 1; i < parsed.tokens.size(); ++i) {
    args.push_back(parsed.tokens[i].text);
  }

  const ActionSpec* action = FindWorkspaceActionByCommand(command);
  if (action != nullptr) {
    return operations_.execute_action(action->id, args, ActionSource::Command);
  }

  const auto plugin_result = operations_.execute_plugin_command(command, args);
  if (plugin_result.handled) {
    if (!plugin_result.feedback.empty()) {
      SetFeedback(plugin_result.feedback);
    }
    return true;
  }
  if (!plugin_result.error.empty()) {
    SetFeedback(plugin_result.error);
    return false;
  }

  SetFeedback("Unknown command: " + command);
  return false;
}

}  // namespace microide::workspace
