#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "editor/TextViewport.h"
#include "project/ProjectSearchService.h"
#include "render/Theme.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/WorkspaceTerminalSelection.h"

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

struct WorkspaceTabTextModel {
  std::string display_title;
  std::string tooltip_label;
};

enum class GitSidebarSection {
  Modified,
  Outgoing,
};

enum class GitSidebarLineKind {
  Header,
  Entry,
  Empty,
};

struct GitSidebarLineSpec {
  GitSidebarLineKind kind = GitSidebarLineKind::Empty;
  GitSidebarSection section = GitSidebarSection::Modified;
  std::string label;
  int entry_index = -1;
};

struct GitSidebarEntryTextModel {
  std::string primary_label;
  std::string secondary_label;
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

std::vector<std::string> SplitSyntaxLines(std::string_view text);
std::string SerializeLines(const std::vector<std::string>& lines,
                           editor::TextViewport::LineEnding line_ending);
editor::TextViewport::LineEnding DetectLineEnding(std::string_view text);
bool RemoveLastUtf8Codepoint(std::string* text);
std::size_t Utf8ByteOffsetForCodepointCount(std::string_view text, std::size_t codepoint_count);
std::size_t Utf8CodepointCount(std::string_view text);
std::string CollapseWhitespace(std::string_view text);
bool QuerySupportsLiteralReplace(std::string_view query);
bool UsesCaseSensitiveLiteralMatch(std::string_view query);
std::size_t ReplaceLiteralMatchesInText(std::string& content,
                                        std::string_view query,
                                        std::string_view replacement,
                                        bool case_sensitive);
std::vector<editor::SelectionRange> FindLiteralSearchMatches(
    const std::vector<std::string>& lines,
    std::string_view query);

ParsedCommandLine ParseCommandLine(std::string_view input);
bool StartsWith(std::string_view text, std::string_view prefix);
bool EndsWith(std::string_view text, std::string_view suffix);
std::string ToLower(std::string_view text);
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
std::string RelativePathLabel(const std::filesystem::path& root,
                              const std::filesystem::path& path);
bool PathEqualsOrWithin(const std::filesystem::path& candidate,
                        const std::filesystem::path& root);
std::filesystem::path ReplacePathPrefix(const std::filesystem::path& path,
                                        const std::filesystem::path& old_prefix,
                                        const std::filesystem::path& new_prefix);
std::vector<GitSidebarLineSpec> BuildGitSidebarLineSpecs(
    const std::vector<GitSidebarSection>& entry_sections,
    bool git_repo_available,
    std::string_view git_base_ref,
    std::string_view git_base_label);
std::optional<std::size_t> FindSelectedGitSidebarLineIndex(
    const std::vector<GitSidebarLineSpec>& lines,
    std::size_t selected_entry_index);
std::vector<int> BuildProjectSearchResultLineMap(
    const std::vector<project::ProjectSearchResult>& results);
int FindProjectSearchResultLine(const std::vector<int>& line_map, std::size_t result_index);

std::string ProjectStateDirectoryName(const std::filesystem::path& project_root);
GitSidebarEntryTextModel BuildGitSidebarEntryTextModel(const std::filesystem::path& relative_path,
                                                       bool staged);
WorkspaceTabTextModel BuildWorkspaceTabTextModel(const std::filesystem::path& project_root,
                                                 const std::filesystem::path& path,
                                                 std::string_view fallback_title,
                                                 bool dirty);
std::string BuildEditorBreadcrumbLabel(const std::filesystem::path& project_root,
                                       const std::filesystem::path& path,
                                       bool placeholder);
std::string BuildCompareBreadcrumbLabel(const std::filesystem::path& project_root,
                                        const std::filesystem::path& path,
                                        std::string_view left_label,
                                        std::string_view right_label);
std::string BuildMergeBreadcrumbLabel(const std::filesystem::path& project_root,
                                      const std::filesystem::path& output_path,
                                      std::string_view incoming_label,
                                      std::string_view current_label);
std::optional<SDL_Color> ParseProjectColor(std::string_view text);
std::string FormatProjectColor(SDL_Color color);
SDL_Color DefaultProjectBaseColor(const std::filesystem::path& project_root);
void ApplyProjectAccent(render::Theme& theme, SDL_Color accent);

}  // namespace microide::workspace
