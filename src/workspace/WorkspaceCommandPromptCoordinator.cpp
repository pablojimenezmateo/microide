#include "workspace/WorkspaceCommandPromptCoordinator.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "workspace/WorkspaceActionCoordinator.h"
#include "workspace/WorkspaceConstants.h"
#include "workspace/WorkspaceCommandRegistry.h"
#include "workspace/WorkspaceSidebarRegistry.h"
#include "workspace/WorkspaceShellShared.h"

namespace microide::workspace {

WorkspaceShell::CommandPromptCoordinator::CommandPromptCoordinator(WorkspaceShell& shell)
    : shell_(shell) {}

void WorkspaceShell::CommandPromptCoordinator::ResetSessionState() {
  shell_.command_.history_index.reset();
  shell_.command_.history_pending_input.clear();
  ClearFeedback();
}

void WorkspaceShell::CommandPromptCoordinator::ClearFeedback() {
  shell_.command_.feedback_text.clear();
}

void WorkspaceShell::CommandPromptCoordinator::SetFeedback(std::string feedback) {
  shell_.command_.feedback_text = std::move(feedback);
}

bool WorkspaceShell::CommandPromptCoordinator::RejectAction(ActionSource source,
                                                            std::string feedback) {
  if (source != ActionSource::Command) {
    return true;
  }
  SetFeedback(std::move(feedback));
  return false;
}

void WorkspaceShell::CommandPromptCoordinator::AppendInput(std::string_view input) {
  shell_.command_.input.append(input);
  shell_.command_.history_index.reset();
  shell_.command_.history_pending_input.clear();
  ClearFeedback();
}

void WorkspaceShell::CommandPromptCoordinator::PushHistory(std::string command_line) {
  if (command_line.empty()) {
    return;
  }
  if (!shell_.command_.history.empty() && shell_.command_.history.back() == command_line) {
    return;
  }

  shell_.command_.history.push_back(std::move(command_line));
  if (shell_.command_.history.size() > 64) {
    shell_.command_.history.erase(shell_.command_.history.begin());
  }
}

void WorkspaceShell::CommandPromptCoordinator::StepHistory(int delta) {
  if (delta == 0 || shell_.command_.history.empty()) {
    return;
  }

  if (!shell_.command_.history_index.has_value()) {
    if (delta > 0) {
      return;
    }
    shell_.command_.history_pending_input = shell_.command_.input;
    shell_.command_.history_index = shell_.command_.history.size() - 1;
  } else if (delta < 0) {
    if (*shell_.command_.history_index > 0) {
      --(*shell_.command_.history_index);
    }
  } else if (*shell_.command_.history_index + 1 < shell_.command_.history.size()) {
    ++(*shell_.command_.history_index);
  } else {
    shell_.command_.history_index.reset();
    shell_.command_.input = shell_.command_.history_pending_input;
    shell_.command_.history_pending_input.clear();
    ClearFeedback();
    return;
  }

  shell_.command_.input = shell_.command_.history[*shell_.command_.history_index];
  ClearFeedback();
}

void WorkspaceShell::CommandPromptCoordinator::CompleteInput() {
  const ParsedCommandLine parsed = ParseCommandLine(shell_.command_.input);
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
                                        ? shell_.command_.input.size()
                                        : parsed.tokens.back().start;
  const std::filesystem::path completion_root =
      shell_.project_root_.empty() ? std::filesystem::current_path() : shell_.project_root_;
  std::vector<std::string> command_names = WorkspaceCommandNames();
  const auto& plugin_command_names = shell_.plugin_host_.CommandNames();
  command_names.insert(command_names.end(), plugin_command_names.begin(), plugin_command_names.end());

  std::vector<CommandCompletionCandidate> candidates;
  if (active_index == 0) {
    candidates = CompleteFromValues(active_prefix, command_names);
  } else if (command == "colorscheme" && active_index == 1) {
    candidates = CompleteFromValues(active_prefix, shell_.available_colorscheme_names_);
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

    for (std::size_t i = 0; i < shell_.open_tabs_.size(); ++i) {
      add_candidate(std::to_string(i + 1));
      add_candidate(shell_.open_tabs_[i].title);
      add_candidate(RelativePathLabel(shell_.project_root_, shell_.open_tabs_[i].path));
    }
  } else if (command == "tree" || command == "files") {
    candidates = CompletePath(completion_root, active_prefix, true);
  } else if (command == "sidebar-show" || command == "sidebar-toggle") {
    if (active_index == 1) {
      std::vector<std::string> sidebar_names = BuiltinSidebarToolNames();
      for (const auto& provider : shell_.plugin_host_.SidebarProviders()) {
        sidebar_names.push_back(provider.id);
      }
      std::sort(sidebar_names.begin(), sidebar_names.end());
      sidebar_names.erase(std::unique(sidebar_names.begin(), sidebar_names.end()),
                          sidebar_names.end());
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
    shell_.command_.input.erase(replace_start);
    shell_.command_.input += replacement;
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

bool WorkspaceShell::CommandPromptCoordinator::HandleKeyDown(const SDL_KeyboardEvent& event) {
  switch (event.key) {
    case SDLK_ESCAPE: {
      const bool bottom_panel_was_visible = shell_.BottomPanelVisible();
      shell_.surface_.command_mode = false;
      shell_.command_.input.clear();
      ResetSessionState();
      shell_.RequestCommandModeTransitionRedraw(bottom_panel_was_visible);
      return true;
    }
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
      if (shell_.command_.input.empty() || ExecuteCommandLine(shell_.command_.input)) {
        const bool bottom_panel_was_visible = shell_.BottomPanelVisible();
        shell_.surface_.command_mode = false;
        shell_.command_.input.clear();
        ResetSessionState();
        shell_.RequestCommandModeTransitionRedraw(bottom_panel_was_visible);
      }
      return true;
    case SDLK_BACKSPACE:
      RemoveLastUtf8Codepoint(&shell_.command_.input);
      shell_.command_.history_index.reset();
      shell_.command_.history_pending_input.clear();
      ClearFeedback();
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

std::string WorkspaceShell::CommandPromptCoordinator::PromptStatusText(
    const WorkspaceShell& shell) {
  if (!shell.command_.feedback_text.empty()) {
    return shell.command_.feedback_text;
  }
  if (shell.command_.history_index.has_value()) {
    return "History " + std::to_string(*shell.command_.history_index + 1) + " / " +
           std::to_string(shell.command_.history.size()) +
           "  |  Enter run  Esc cancel  Tab complete";
  }
  return "Enter run  Esc cancel  Up/Down history  Tab complete";
}

bool WorkspaceShell::CommandPromptCoordinator::ExecuteCommandLine(
    const std::string& command_line) {
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
    return ActionCoordinator(shell_).Execute(action->id, args, ActionSource::Command);
  }

  const std::size_t message_count_before = shell_.plugin_host_.Messages().size();
  std::string plugin_error;
  if (shell_.plugin_host_.ExecuteCommand(command, args, &plugin_error)) {
    if (shell_.plugin_host_.Messages().size() > message_count_before) {
      SetFeedback(shell_.plugin_host_.Messages().back());
    }
    return true;
  }
  if (!plugin_error.empty()) {
    SetFeedback(std::move(plugin_error));
    return false;
  }

  SetFeedback("Unknown command: " + command);
  return false;
}

}  // namespace microide::workspace
