#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "workspace/WorkspaceTextSearch.h"

namespace microide::workspace {

struct ParsedCommandToken {
  std::string text;
  std::size_t start = 0;
};

struct ParsedCommandLine {
  std::vector<ParsedCommandToken> tokens;
  bool trailing_space = false;
  bool dangling_escape = false;
  char open_quote = '\0';
};

struct CommandCompletionCandidate {
  std::string value;
  bool append_space = true;
};

inline constexpr std::array<float, 10> kUiScalePresets = {
    0.75f,
    1.0f,
    1.25f,
    1.5f,
    1.75f,
    2.0f,
    2.25f,
    2.5f,
    2.75f,
    3.0f,
};

inline constexpr float kMinUiScale = kUiScalePresets.front();
inline constexpr float kMaxUiScale = kUiScalePresets.back();

std::string UiScaleLabel(float scale);
std::optional<float> ParseUiScaleValue(std::string_view text);
float StepUiScale(float current_scale, int delta);

ParsedCommandLine ParseCommandLine(std::string_view input);
std::string CommonPrefix(const std::vector<CommandCompletionCandidate>& candidates);
bool CommandArgNeedsQuoting(std::string_view argument);
std::string QuoteCommandArg(std::string_view argument);
std::string FormatCommandCompletionToken(const CommandCompletionCandidate& candidate);

template <std::size_t N>
std::vector<CommandCompletionCandidate> CompleteFromList(
    std::string_view prefix,
    const std::array<std::string_view, N>& suggestions,
    bool append_space = true) {
  std::vector<CommandCompletionCandidate> matches;
  for (std::string_view suggestion : suggestions) {
    if (!StartsWith(suggestion, prefix)) {
      continue;
    }
    matches.push_back(CommandCompletionCandidate{std::string(suggestion), append_space});
  }
  return matches;
}

std::vector<CommandCompletionCandidate> CompleteFromValues(std::string_view prefix,
                                                           const std::vector<std::string>& values,
                                                           bool append_space = true);
std::vector<CommandCompletionCandidate> CompletePath(const std::filesystem::path& project_root,
                                                     std::string_view token,
                                                     bool directories_only);
std::string JoinCommandArguments(const std::vector<std::string>& args, std::size_t start_index);

}  // namespace microide::workspace
