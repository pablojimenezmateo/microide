#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "compare/CompareModel.h"
#include "compare/MergeModel.h"
#include "editor/TextViewport.h"
#include "project/ProjectSearchService.h"
#include "render/Theme.h"
#include "terminal/TerminalSession.h"

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

struct PersistedEditorViewState {
  std::size_t leaf_id = 0;
  std::filesystem::path path;
  std::size_t cursor_line = 0;
  std::size_t cursor_column = 0;
  std::size_t scroll_line = 0;
  std::size_t horizontal_scroll = 0;
};

struct PersistedSplitNodeState {
  std::vector<std::size_t> path;
  std::string orientation;
  float size_fraction = 1.0f;
  std::size_t leaf_id = 0;
};

struct PersistedEditorTabState {
  std::string kind = "editor";
  std::size_t active_leaf_id = 0;
  std::vector<PersistedEditorViewState> views;
  std::vector<PersistedSplitNodeState> split_nodes;
  std::filesystem::path compare_path;
  std::filesystem::path compare_left_path;
  std::filesystem::path compare_right_path;
  std::string compare_commit_hash;
  std::string compare_commit_short_hash;
  std::string compare_right_ref;
  std::string compare_right_label;
  std::size_t compare_selected_row = 0;
  std::size_t compare_scroll_row = 0;
  std::size_t compare_horizontal_scroll = 0;
  std::filesystem::path merge_base_path;
  std::filesystem::path merge_incoming_path;
  std::filesystem::path merge_current_path;
  std::filesystem::path merge_output_path;
  std::size_t merge_selected_hunk = 0;
  std::size_t merge_scroll_row = 0;
  std::size_t merge_horizontal_scroll = 0;
  float merge_left_divider_fraction = 1.0f / 3.0f;
  float merge_right_divider_fraction = 2.0f / 3.0f;
  std::vector<std::string> merge_hunk_choices;
};

struct WorkspaceTabTextModel {
  std::string display_title;
  std::string tooltip_label;
};

struct PersistedUserConfigState {
  float ui_scale = 1.0f;
};

struct PersistedProjectConfigState {
  std::size_t editor_tab_size = 4;
  std::size_t editor_indent_width = 4;
  bool editor_soft_tabs = false;
  std::string colorscheme_name = "default";
  std::optional<SDL_Color> project_base_color;
};

struct PersistedProjectSessionState {
  bool sidebar_visible = true;
  float sidebar_width = 288.0f;
  float bottom_panel_height = 184.0f;
  std::size_t active_tab_index = 0;
  std::vector<PersistedEditorTabState> tabs;
};

struct PersistedWorkspaceSessionState {
  std::vector<std::filesystem::path> project_roots;
  std::size_t active_project_index = 0;
};

struct WorkspaceLayout {
  SDL_FRect full{};
  SDL_FRect menu_bar{};
  SDL_FRect project_tab_strip{};
  SDL_FRect tab_strip{};
  SDL_FRect bottom_panel{};
  SDL_FRect content{};
  SDL_FRect sidebar{};
  SDL_FRect editor_area{};
  SDL_FRect breadcrumb{};
  SDL_FRect editor_surface{};
};

struct ScrollbarGeometry {
  SDL_FRect track{};
  SDL_FRect thumb{};
  float total_units = 0.0f;
  float visible_units = 0.0f;
  float scroll_units = 0.0f;
  bool vertical = true;
};

struct CompareScrollbarMarker {
  compare::CompareRowKind kind = compare::CompareRowKind::Unchanged;
  int start_row = 0;
  int end_row = 0;
  SDL_FRect rect{};
};

struct MergeScrollbarMarkerInput {
  int start_row = 0;
  int end_row = 0;
  compare::MergeChoice choice = compare::MergeChoice::Base;
  bool valid = true;
};

struct MergeScrollbarMarker {
  int start_row = 0;
  int end_row = 0;
  compare::MergeChoice choice = compare::MergeChoice::Base;
  bool valid = true;
  SDL_FRect rect{};
};

struct StripSlotLayout {
  std::size_t index = 0;
  float x = 0.0f;
  float width = 0.0f;
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

struct TerminalSelectionPoint {
  std::size_t row = 0;
  std::size_t column = 0;
};

struct TerminalSelectionBounds {
  TerminalSelectionPoint start{};
  TerminalSelectionPoint end{};
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

std::string EncodeSessionNodePath(const std::vector<std::size_t>& path);
std::optional<std::vector<std::size_t>> DecodeSessionNodePath(std::string_view text);

std::vector<std::string> SplitSyntaxLines(std::string_view text);
std::optional<std::string> ReadFileText(const std::filesystem::path& path);
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
SDL_FRect MakeRect(float x, float y, float w, float h);
WorkspaceLayout ComputeLayout(float window_width,
                              float window_height,
                              bool sidebar_visible,
                              bool bottom_panel_visible,
                              float sidebar_width,
                              float bottom_panel_height);
bool Contains(const SDL_FRect& rect, float x, float y);
float ClampSidebarWidth(float width, float window_width);
float ClampBottomPanelHeight(float height, float window_height);
int BottomPanelVisibleRowsForHeight(float panel_height, float line_height, bool command_mode);
int TailScrollRowForContent(std::size_t line_count, int visible_rows);
int ClampScrollRowToContent(int scroll_row, std::size_t line_count, int visible_rows);
SDL_FRect SidebarResizeHandleRect(const WorkspaceLayout& layout);
SDL_FRect BottomPanelResizeHandleRect(const WorkspaceLayout& layout);
float BottomPanelCommandReservedHeight(bool command_mode);
SDL_FRect BottomPanelContentRect(const WorkspaceLayout& layout, bool command_mode);
SDL_FRect BottomPanelCommandAreaRect(const WorkspaceLayout& layout);
SDL_FRect BottomPanelCommandPromptRect(const WorkspaceLayout& layout);
std::optional<TerminalSelectionBounds> NormalizeTerminalSelection(
    std::optional<TerminalSelectionPoint> anchor,
    std::optional<TerminalSelectionPoint> head);
terminal::TerminalSession::MouseButton TerminalMouseButtonFromSdl(Uint8 button);
std::string ExtractTerminalSelectionText(const std::vector<terminal::TerminalLine>& lines,
                                         const TerminalSelectionBounds& selection);
bool TerminalSelectionContainsCell(const TerminalSelectionBounds& selection,
                                   std::size_t row,
                                   std::size_t column);
std::optional<SDL_FRect> ComputeScrollbarThumb(const SDL_FRect& track,
                                               float total_units,
                                               float visible_units,
                                               float scroll_units,
                                               bool vertical);
std::optional<ScrollbarGeometry> MakeVerticalScrollbarGeometry(const SDL_FRect& area,
                                                              float total_units,
                                                              float visible_units,
                                                              float scroll_units,
                                                              bool reserve_horizontal = false);
std::optional<ScrollbarGeometry> MakeHorizontalScrollbarGeometry(const SDL_FRect& area,
                                                                float total_units,
                                                                float visible_units,
                                                                float scroll_units,
                                                                bool reserve_vertical = false);
float ScrollUnitsForPointer(const ScrollbarGeometry& geometry,
                            float pointer_coordinate,
                            float grab_offset);
std::vector<CompareScrollbarMarker> BuildCompareScrollbarMarkers(
    const SDL_FRect& track,
    const compare::CompareModel& model);
std::vector<MergeScrollbarMarker> BuildMergeScrollbarMarkers(
    const SDL_FRect& track,
    std::size_t total_rows,
    const std::vector<MergeScrollbarMarkerInput>& inputs);
std::vector<StripSlotLayout> ComputeVisibleStripLayouts(const std::vector<float>& widths,
                                                        float start_x,
                                                        float gap,
                                                        float max_x,
                                                        std::size_t first_index);
std::size_t EnsureVisibleStripIndex(const std::vector<float>& widths,
                                    float start_x,
                                    float gap,
                                    float max_x,
                                    std::size_t current_first_index,
                                    std::size_t active_index);
SDL_FRect ComputeOverlaySurfaceRect(const SDL_FRect& editor_area);
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
WorkspaceTabTextModel BuildWorkspaceTabTextModel(const std::filesystem::path& project_root,
                                                 const std::filesystem::path& path,
                                                 std::string_view fallback_title,
                                                 bool dirty);
std::string BuildEditorBreadcrumbLabel(const std::filesystem::path& project_root,
                                       const std::filesystem::path& path,
                                       bool placeholder,
                                       bool large_file_mode);
std::string BuildCompareBreadcrumbLabel(const std::filesystem::path& project_root,
                                        const std::filesystem::path& path,
                                        std::string_view left_label,
                                        std::string_view right_label);
std::string BuildMergeBreadcrumbLabel(const std::filesystem::path& project_root,
                                      const std::filesystem::path& output_path,
                                      std::string_view incoming_label,
                                      std::string_view current_label);
bool ParseUserConfigText(std::string_view text, PersistedUserConfigState* state);
std::string SerializeUserConfig(const PersistedUserConfigState& state);
bool ParseProjectConfigText(std::string_view text, PersistedProjectConfigState* state);
std::string SerializeProjectConfig(const PersistedProjectConfigState& state);
bool ParseProjectSessionText(std::string_view text, PersistedProjectSessionState* state);
std::string SerializeProjectSession(const PersistedProjectSessionState& state);
bool ParseWorkspaceSessionText(std::string_view text, PersistedWorkspaceSessionState* state);
std::string SerializeWorkspaceSession(const PersistedWorkspaceSessionState& state);
std::optional<SDL_Color> ParseProjectColor(std::string_view text);
std::string FormatProjectColor(SDL_Color color);
SDL_Color DefaultProjectBaseColor(const std::filesystem::path& project_root);
void ApplyProjectAccent(render::Theme& theme, SDL_Color accent);

}  // namespace microide::workspace
