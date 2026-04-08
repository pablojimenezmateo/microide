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
  command_history_index_.reset();
  command_history_pending_input_.clear();
  ClearCommandCompletionFeedback();
}

void WorkspaceShell::ClearCommandCompletionFeedback() {
  command_completion_feedback_.clear();
}

void WorkspaceShell::PushCommandHistory(std::string command_line) {
  if (command_line.empty()) {
    return;
  }
  if (!command_history_.empty() && command_history_.back() == command_line) {
    return;
  }

  command_history_.push_back(std::move(command_line));
  if (command_history_.size() > 64) {
    command_history_.erase(command_history_.begin());
  }
}

void WorkspaceShell::StepCommandHistory(int delta) {
  if (delta == 0 || command_history_.empty()) {
    return;
  }

  if (!command_history_index_.has_value()) {
    if (delta > 0) {
      return;
    }
    command_history_pending_input_ = command_input_;
    command_history_index_ = command_history_.size() - 1;
  } else if (delta < 0) {
    if (*command_history_index_ > 0) {
      --(*command_history_index_);
    }
  } else if (*command_history_index_ + 1 < command_history_.size()) {
    ++(*command_history_index_);
  } else {
    command_history_index_.reset();
    command_input_ = command_history_pending_input_;
    command_history_pending_input_.clear();
    ClearCommandCompletionFeedback();
    return;
  }

  command_input_ = command_history_[*command_history_index_];
  ClearCommandCompletionFeedback();
}

void WorkspaceShell::CompleteCommandInput() {
  const ParsedCommandLine parsed = ParseCommandLine(command_input_);
  if (parsed.dangling_escape) {
    command_completion_feedback_ = "Command completion stopped at a trailing escape";
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
      starts_new_token || parsed.tokens.empty() ? command_input_.size() : parsed.tokens.back().start;
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
             command == "term" || command == "vsplit" || command == "hsplit") {
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
  } else if (command == "help") {
    candidates = CompleteFromValues(active_prefix, command_names);
  }

  if (candidates.empty()) {
    command_completion_feedback_ = "No completion matches";
    return;
  }

  const std::string common_prefix = CommonPrefix(candidates);
  const bool can_extend_prefix = common_prefix.size() > active_prefix.size();
  if (candidates.size() == 1 || can_extend_prefix) {
    CommandCompletionCandidate candidate =
        candidates.size() == 1 ? candidates.front()
                               : CommandCompletionCandidate{common_prefix, false};
    std::string replacement = FormatCommandCompletionToken(candidate);
    command_input_.erase(replace_start);
    command_input_ += replacement;
  }

  if (candidates.size() == 1) {
    command_completion_feedback_ = "Completed " + candidates.front().value;
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
  command_completion_feedback_ = std::move(matches);
}

std::string WorkspaceShell::CommandPromptStatusText() const {
  if (!command_completion_feedback_.empty()) {
    return command_completion_feedback_;
  }
  if (command_history_index_.has_value()) {
    return "History " + std::to_string(*command_history_index_ + 1) + " / " +
           std::to_string(command_history_.size()) + "  |  Enter run  Esc cancel  Tab complete";
  }
  return "Enter run  Esc cancel  Up/Down history  Tab complete";
}

bool WorkspaceShell::ExecuteCommand(const std::string& command_line) {
  const ParsedCommandLine parsed = ParseCommandLine(command_line);
  if (parsed.dangling_escape) {
    command_completion_feedback_ = "Command parse error: trailing escape";
    LogMessage(command_completion_feedback_);
    return false;
  }
  if (parsed.open_quote != '\0') {
    command_completion_feedback_ = std::string("Command parse error: unterminated ") +
                                   (parsed.open_quote == '\'' ? "single" : "double") + " quote";
    LogMessage(command_completion_feedback_);
    return false;
  }
  if (parsed.tokens.empty()) {
    return true;
  }

  PushCommandHistory(command_line);
  ClearCommandCompletionFeedback();
  const std::string& command = parsed.tokens.front().text;
  const ActionSpec* action = FindActionByCommand(command);
  if (action == nullptr) {
    LogMessage("Unknown command: " + command);
    return true;
  }

  std::vector<std::string> args;
  args.reserve(parsed.tokens.size() - 1);
  for (std::size_t i = 1; i < parsed.tokens.size(); ++i) {
    args.push_back(parsed.tokens[i].text);
  }
  return ExecuteAction(action->id, args, ActionSource::Command);
}

void WorkspaceShell::LogMessage(std::string message) {
  if (message.empty()) {
    return;
  }
  status_message_ = std::move(message);
}

}  // namespace microide::workspace
