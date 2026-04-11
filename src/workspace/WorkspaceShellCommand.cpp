#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "workspace/WorkspaceShellShared.h"

namespace microide::workspace {

namespace {

constexpr std::array<std::string_view, 3> kSidebarToolNames = {
    "git",
    "search",
    "tree",
};

constexpr std::array<std::string_view, 3> kFocusTargetNames = {
    "editor",
    "panel",
    "sidebar",
};

constexpr std::array<std::string_view, 2> kToggleValues = {
    "off",
    "on",
};

constexpr std::array<std::string_view, 3> kUiScaleCommands = {
    "down",
    "reset",
    "up",
};

}  // namespace

void WorkspaceShell::ResetCommandSessionState() {
  command_.history_index.reset();
  command_.history_pending_input.clear();
  ClearCommandFeedback();
}

void WorkspaceShell::ClearCommandFeedback() {
  command_.feedback_text.clear();
}

bool WorkspaceShell::RejectCommandAction(ActionSource source, std::string feedback) {
  if (source != ActionSource::Command) {
    return true;
  }
  command_.feedback_text = std::move(feedback);
  return false;
}

void WorkspaceShell::PushCommandHistory(std::string command_line) {
  if (command_line.empty()) {
    return;
  }
  if (!command_.history.empty() && command_.history.back() == command_line) {
    return;
  }

  command_.history.push_back(std::move(command_line));
  if (command_.history.size() > 64) {
    command_.history.erase(command_.history.begin());
  }
}

void WorkspaceShell::StepCommandHistory(int delta) {
  if (delta == 0 || command_.history.empty()) {
    return;
  }

  if (!command_.history_index.has_value()) {
    if (delta > 0) {
      return;
    }
    command_.history_pending_input = command_.input;
    command_.history_index = command_.history.size() - 1;
  } else if (delta < 0) {
    if (*command_.history_index > 0) {
      --(*command_.history_index);
    }
  } else if (*command_.history_index + 1 < command_.history.size()) {
    ++(*command_.history_index);
  } else {
    command_.history_index.reset();
    command_.input = command_.history_pending_input;
    command_.history_pending_input.clear();
    ClearCommandFeedback();
    return;
  }

  command_.input = command_.history[*command_.history_index];
  ClearCommandFeedback();
}

void WorkspaceShell::CompleteCommandInput() {
  const ParsedCommandLine parsed = ParseCommandLine(command_.input);
  if (parsed.dangling_escape) {
    command_.feedback_text = "Command completion stopped at a trailing escape";
    return;
  }

  const bool starts_new_token = parsed.open_quote == '\0' && parsed.trailing_space;
  const std::size_t active_index =
      starts_new_token ? parsed.tokens.size()
                       : (parsed.tokens.empty() ? 0 : parsed.tokens.size() - 1);
  const std::string command = parsed.tokens.empty() ? std::string{} : parsed.tokens.front().text;
  const std::string active_prefix =
      starts_new_token || parsed.tokens.empty() ? std::string{}
                                                : parsed.tokens.back().text;
  const std::size_t replace_start =
      starts_new_token || parsed.tokens.empty() ? command_.input.size() : parsed.tokens.back().start;
  const std::filesystem::path completion_root =
      project_root_.empty() ? std::filesystem::current_path() : project_root_;
  const std::vector<std::string>& command_names = CommandNames();

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

    for (std::size_t i = 0; i < open_tabs_.size(); ++i) {
      add_candidate(std::to_string(i + 1));
      add_candidate(open_tabs_[i].title);
      add_candidate(RelativePathLabel(project_root_, open_tabs_[i].path));
    }
  } else if (command == "tree" || command == "files") {
    candidates = CompletePath(completion_root, active_prefix, true);
  } else if (command == "sidebar-show" || command == "sidebar-toggle") {
    if (active_index == 1) {
      candidates = CompleteFromList(active_prefix, kSidebarToolNames);
    } else if (parsed.tokens.size() >= 2 && parsed.tokens[1].text == "tree" && active_index == 2) {
      candidates = CompletePath(completion_root, active_prefix, true);
    }
  } else if (command == "soft-tabs" && active_index == 1) {
    candidates = CompleteFromList(active_prefix, kToggleValues);
  } else if (command == "ui-scale" && active_index == 1) {
    candidates = CompleteFromList(active_prefix, kUiScaleCommands);
  }

  if (candidates.empty()) {
    command_.feedback_text = "No completion matches";
    return;
  }

  const std::string common_prefix = CommonPrefix(candidates);
  const bool can_extend_prefix = common_prefix.size() > active_prefix.size();
  if (candidates.size() == 1 || can_extend_prefix) {
    CommandCompletionCandidate candidate =
        candidates.size() == 1 ? candidates.front()
                               : CommandCompletionCandidate{common_prefix, false};
    std::string replacement = FormatCommandCompletionToken(candidate);
    command_.input.erase(replace_start);
    command_.input += replacement;
  }

  if (candidates.size() == 1) {
    command_.feedback_text = "Completed " + candidates.front().value;
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
  command_.feedback_text = std::move(matches);
}

std::string WorkspaceShell::CommandPromptStatusText() const {
  if (!command_.feedback_text.empty()) {
    return command_.feedback_text;
  }
  if (command_.history_index.has_value()) {
    return "History " + std::to_string(*command_.history_index + 1) + " / " +
           std::to_string(command_.history.size()) + "  |  Enter run  Esc cancel  Tab complete";
  }
  return "Enter run  Esc cancel  Up/Down history  Tab complete";
}

bool WorkspaceShell::ExecuteCommand(const std::string& command_line) {
  const ParsedCommandLine parsed = ParseCommandLine(command_line);
  if (parsed.dangling_escape) {
    command_.feedback_text = "Command parse error: trailing escape";
    return false;
  }
  if (parsed.open_quote != '\0') {
    command_.feedback_text = std::string("Command parse error: unterminated ") +
                             (parsed.open_quote == '\'' ? "single" : "double") + " quote";
    return false;
  }
  if (parsed.tokens.empty()) {
    return true;
  }

  PushCommandHistory(command_line);
  ClearCommandFeedback();
  const std::string& command = parsed.tokens.front().text;
  const ActionSpec* action = FindActionByCommand(command);
  if (action == nullptr) {
    command_.feedback_text = "Unknown command: " + command;
    return false;
  }

  std::vector<std::string> args;
  args.reserve(parsed.tokens.size() - 1);
  for (std::size_t i = 1; i < parsed.tokens.size(); ++i) {
    args.push_back(parsed.tokens[i].text);
  }
  return ExecuteAction(action->id, args, ActionSource::Command);
}

}  // namespace microide::workspace
