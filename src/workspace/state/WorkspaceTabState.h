#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "compare/BranchReviewStateTypes.h"
#include "platform/ProcessLauncher.h"
#include "project/GitCompareService.h"
#include "compare/CompareModel.h"
#include "compare/ComparePresentationModel.h"
#include "compare/CompareReviewTypes.h"
#include "compare/CompareSemanticMetadata.h"
#include "compare/MergeConflictKind.h"
#include "compare/MergeModel.h"
#include "editor/AsyncBufferWork.h"
#include "editor/FoldingModel.h"
#include "editor/RuntimeSyntaxRegistry.h"
#include "editor/SnippetEngine.h"
#include "editor/TextLayout.h"
#include "editor/BracketScanner.h"
#include "editor/TextViewport.h"
#include "terminal/TerminalSession.h"
#include "util/PathMatch.h"
#include "workspace/DiffWrapLayout.h"
#include "workspace/render/OverviewRuler.h"
#include "workspace/state/SurfaceTokenWindow.h"
#include "workspace/TerminalPaneLayout.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/WorkspaceTerminalSelection.h"

namespace microide::workspace {

enum class CompareHoverKind {
  CollapsedContextPreviousAction,
  CollapsedContextAllAction,
  CollapsedContextNextAction,
  HunkToggleReviewed,
  HunkComment,
  NoteEdit,
  NoteDelete,
  HeaderPreviousChange,
  HeaderNextChange,
  HeaderToggleFileReviewed,
};

struct CompareHoverState {
  CompareHoverKind kind = CompareHoverKind::CollapsedContextAllAction;
  std::size_t presentation_row = 0;
  std::size_t collapsed_run_start_model_row = 0;
  std::size_t collapsed_run_length = 0;
  int hunk = -1;
  bool operator==(const CompareHoverState&) const = default;
};

struct CompareReviewHeaderState {
  std::string summary_line;
};

struct CompareVisibleLayoutCacheKey {
  std::size_t model_row = 0;
  std::size_t horizontal_scroll = 0;
  std::size_t visible_columns = 0;
  std::size_t tab_size = 0;
  bool right_side = false;

  bool operator==(const CompareVisibleLayoutCacheKey&) const = default;
};

struct CompareVisibleLayoutCacheKeyHash {
  std::size_t operator()(const CompareVisibleLayoutCacheKey& key) const noexcept {
    std::size_t h = key.model_row;
    h ^= key.horizontal_scroll * 2654435761ULL + 0x9e3779b9ULL + (h << 6) + (h >> 2);
    h ^= key.visible_columns * 2654435761ULL + 0x9e3779b9ULL + (h << 6) + (h >> 2);
    h ^= key.tab_size * 2654435761ULL + 0x9e3779b9ULL + (h << 6) + (h >> 2);
    h ^= static_cast<std::size_t>(key.right_side) + 0x9e3779b9ULL + (h << 6) + (h >> 2);
    return h;
  }
};

struct CompareVisibleLayoutCache {
  static constexpr std::size_t kLimit = 512;
  static constexpr std::size_t kTableSize = 1024;

  std::uint64_t model_revision = 0;
  std::size_t live_count = 0;
  std::vector<editor::LayoutLine> layouts;
  std::vector<CompareVisibleLayoutCacheKey> keys;
  std::vector<std::uint32_t> table;
};

struct CompareRevisionSides {
  std::optional<project::GitFileContentAtCommit> left;
  std::optional<project::GitFileContentAtCommit> right;
};

struct CompareTabState {
  editor::AsyncBufferWork pending_format_save;
  std::filesystem::path path;
  std::filesystem::path left_path;
  std::filesystem::path right_path;
  std::string title;
  std::string commit_hash;
  std::string right_ref;
  std::string left_label;
  std::string right_label;
  compare::CompareTextBuffer left_content = compare::EmptyCompareText();
  compare::CompareReviewMode review_mode = compare::CompareReviewMode::WorkingTree;
  compare::WorkingTreeStagingView staging_view = compare::WorkingTreeStagingView::Combined;
  compare::BranchReviewTargetIdentity branch_target;
  compare::CompareSemanticFileMetadata semantic_file;
  compare::ComparePresentationModel presentation;
  CompareReviewHeaderState review_header;
  std::vector<compare::BranchReviewHunkMarker> review_hunk_markers;
  std::string review_file_note;
  std::vector<compare::BranchReviewNote> review_detached_notes;
  std::size_t review_reviewed_hunk_count = 0;
  std::uint64_t review_notes_fingerprint = 0;
  compare::BranchReviewTargetIdentity review_markers_built_target;
  std::uint64_t review_markers_built_presentation_revision = 0;
  std::uint64_t review_markers_built_review_revision = 0;
  bool review_markers_valid = false;
  int review_hover_visible_row = -1;
  std::uint64_t presentation_revision = 0;
  compare::CompareBuildOptions build_options;
  bool show_whitespace = false;
  bool opened_from_commit_picker = false;
  std::vector<std::filesystem::path> review_files;
  std::size_t review_file_index = 0;
  compare::CompareModel model;
  editor::TextViewport right_viewport;
  DiffWrapLayout wrap_layout;
  SurfaceTokenWindow left_token_window;
  SurfaceTokenWindow right_token_window;
  std::vector<std::uint32_t> left_line_rows;
  std::vector<std::uint32_t> right_line_rows;
  bool token_windows_stale = true;
  std::uint64_t bracket_match_content_revision = 0;
  std::size_t bracket_match_caret_line = 0;
  std::size_t bracket_match_caret_column = 0;
  std::optional<editor::BracketMatchPair> bracket_match_pair;
  bool bracket_match_valid = false;

  CompareVisibleLayoutCache visible_layouts;
  bool syntax_highlighting_enabled = true;
  std::uint64_t model_revision = 0;
  std::uint64_t token_windows_model_revision = 0;
  std::uint64_t derived_right_content_revision = 0;
  util::LineEnding derived_right_line_ending = util::LineEnding::LF;
  compare::CompareTextBuffer derived_left_content;
  std::size_t derived_left_line_count = 0;
  bool derived_ignore_whitespace = false;
  std::shared_ptr<const compare::DiffSyntax> derived_syntax;
  bool derived_fingerprint_valid = false;
  bool semantic_inputs_valid = false;
  std::size_t semantic_git_entry_signature = 0;
  bool presentation_valid = false;
  std::uint64_t presentation_built_model_revision = 0;
  bool presentation_built_show_whitespace = false;
  bool load_pending = false;
  bool keep_view_on_load = false;
  editor::AsyncBufferWork pending_load;
  bool refresh_owed = false;
  editor::AsyncBufferWork pending_refresh;
  std::optional<CompareHoverState> hover_state;
  std::size_t selected_row = 0;
  int scroll_row = 0;
  std::size_t horizontal_scroll = 0;
  std::size_t max_visual_columns = 0;
  bool scrollbar_marker_cache_valid = false;
  std::uint64_t scrollbar_marker_cache_revision = 0;
  std::size_t scrollbar_marker_cache_rows = 0;
  std::uint64_t scrollbar_marker_cache_theme_token = 0;
  SDL_FRect scrollbar_marker_cache_track{};
  std::vector<overview::Marker> scrollbar_marker_cache;
  std::vector<CompareScrollbarRun> scrollbar_run_scratch;
  std::vector<overview::MarkerInput> scrollbar_marker_input_scratch;
  float divider_fraction = kWorkspaceDefaultCompareDividerFraction;
  bool right_editable = false;
  bool right_view_active = false;
  bool persistable = true;
  bool plain_compare = false;
};

inline bool CompareRowRightTokensAliasLeft(const CompareTabState& tab, std::size_t row) {
  if (row >= tab.model.rows.size()) {
    return false;
  }
  const compare::CompareRow& compare_row = tab.model.rows[row];
  if (compare_row.kind != compare::CompareRowKind::Unchanged || compare_row.left_line <= 0 ||
      compare_row.right_line <= 0 || compare_row.left_text != compare_row.right_text) {
    return false;
  }
  const auto left = static_cast<std::size_t>(compare_row.left_line - 1);
  const auto right = static_cast<std::size_t>(compare_row.right_line - 1);
  if (left >= tab.left_token_window.frontier()) {
    return false;
  }
  return right >= tab.right_token_window.frontier() ||
         tab.left_token_window.StateBefore(left) == tab.right_token_window.StateBefore(right);
}

inline const std::vector<editor::SyntaxTokenKind>& CompareRowLeftTokens(const CompareTabState& tab,
                                                                        const compare::CompareRow& row) {
  static const std::vector<editor::SyntaxTokenKind> kNone;
  return row.left_line > 0 ? tab.left_token_window.Tokens(static_cast<std::size_t>(row.left_line - 1)) : kNone;
}
inline const std::vector<editor::SyntaxTokenKind>& CompareRowRightOwnTokens(const CompareTabState& tab,
                                                                            const compare::CompareRow& row) {
  static const std::vector<editor::SyntaxTokenKind> kNone;
  return row.right_line > 0 ? tab.right_token_window.Tokens(static_cast<std::size_t>(row.right_line - 1)) : kNone;
}

struct MergeTabState {
  editor::AsyncBufferWork pending_format_save;
  std::filesystem::path base_path;
  std::filesystem::path incoming_path;
  std::filesystem::path current_path;
  std::filesystem::path output_path;
  std::string title;
  std::string incoming_label;
  std::string result_label;
  std::string current_label;
  std::string base_label;
  compare::MergeFileConflictMetadata file_conflict;
  std::size_t remaining_conflicted_files = 0;
  std::uint64_t open_index_generation = 0;
  std::optional<std::uint64_t> disk_result_tick;
  bool base_pane_visible = false;
  bool marked_resolved = false;
  bool index_stale = false;
  bool external_result_stale = false;
  bool allow_conflict_marker_override = false;
  bool marker_override_prompt_pending = false;
  bool unresolved_override_prompt_pending = false;
  std::string status_message;
  editor::TextViewport::LineEnding result_line_ending = editor::TextViewport::LineEnding::LF;
  compare::MergeModel model;
  SurfaceTokenWindow incoming_token_window;
  SurfaceTokenWindow current_token_window;
  editor::TextViewport result_viewport;
  DiffWrapLayout wrap_layout;
  std::optional<std::string> persisted_output_baseline;
  std::vector<MergeTrackedConflict> conflicts;
  mutable std::vector<MergeTrackedConflict> visual_conflicts;
  mutable std::uint64_t visual_conflicts_key = 0;
  mutable bool visual_conflicts_valid = false;
  mutable const void* marker_scan_document = nullptr;
  mutable std::uint64_t marker_scan_revision = 0;
  mutable bool marker_scan_valid = false;
  mutable bool marker_scan_complete = false;
  std::optional<MergeHoverState> hover_state;
  std::size_t selected_hunk = 0;
  int scroll_row = 0;
  std::size_t horizontal_scroll = 0;
  std::size_t max_visual_columns = 0;
  std::uint64_t model_revision = 0;
  bool scrollbar_marker_cache_valid = false;
  std::uint64_t scrollbar_marker_cache_revision = 0;
  std::size_t scrollbar_marker_cache_rows = 0;
  std::uint64_t scrollbar_marker_cache_theme_token = 0;
  SDL_FRect scrollbar_marker_cache_track{};
  std::vector<overview::Marker> scrollbar_marker_cache;
  std::vector<overview::MarkerInput> scrollbar_marker_input_scratch;
  std::vector<std::string> preview_lines_cache;
  bool preview_lines_cache_valid = false;
  std::size_t preview_lines_cache_conflict = 0;
  compare::MergeChoice preview_lines_cache_choice = compare::MergeChoice::Base;
  std::uint64_t preview_lines_cache_revision = 0;
  std::string resolver_progress_buffer;
  float left_divider_fraction = kWorkspaceDefaultMergeLeftDividerFraction;
  float right_divider_fraction = kWorkspaceDefaultMergeRightDividerFraction;
  bool persistable = true;
};

struct DeferredTabHandle {
  std::filesystem::path path;
  std::size_t cursor_line = 0;
  std::size_t cursor_column = 0;
  std::size_t scroll_line = 0;
  std::size_t horizontal_scroll = 0;
  std::optional<editor::SelectionRange> selection;
  std::vector<std::size_t> collapsed_fold_openers;

  void ApplyViewStateTo(editor::TextViewport& view) const {
    view.ApplyRestoredViewState(cursor_line, cursor_column, scroll_line, horizontal_scroll,
                                selection);
  }
};

struct EditorTabState {
  editor::TextViewport viewport;
  enum class Content : std::uint8_t {
    Ready,
    Deferred,
    Loading,
    Failed,
  };
  Content content = Content::Ready;
  [[nodiscard]] bool content_pending() const { return content != Content::Ready; }

  DeferredTabHandle restore;
  bool center_cursor_on_load = false;

  editor::AsyncBufferWork pending_format_save;
  editor::AsyncBufferWork pending_load;
  std::unique_ptr<editor::FoldingModel> folding_model =
      std::make_unique<editor::FoldingModel>();
  std::optional<editor::TextPosition> fold_reveal_last_caret;
  std::size_t fold_reveal_last_secondary_count = 0;
  editor::SnippetSessionState snippet_session;
};

enum class EditorReloadEchoGuard { Check, AlreadyResolved };

struct TabEntry {
  enum class Kind {
    Editor,
    Compare,
    Merge,
  };

  using EditorTabState = workspace::EditorTabState;
  using DeferredTabHandle = workspace::DeferredTabHandle;

  Kind kind = Kind::Editor;
  std::filesystem::path path;
  std::string title;
  std::uint32_t untitled_number = 0;
  std::uint64_t stable_id = 0;
  std::optional<EditorTabState> editor_state;
  std::optional<DeferredTabHandle> deferred_handle;
  std::optional<CompareTabState> compare;
  std::optional<MergeTabState> merge;
};

inline std::optional<DeferredTabHandle> EditorTabViewState(const TabEntry& tab) {
  if (tab.kind != TabEntry::Kind::Editor) {
    return std::nullopt;
  }
  if (!tab.editor_state.has_value()) {
    return tab.deferred_handle;
  }
  const EditorTabState& editor = *tab.editor_state;
  if (editor.content_pending()) {
    return editor.restore;
  }
  const editor::TextViewport& view = editor.viewport;
  DeferredTabHandle handle{
      .path = view.path(),
      .cursor_line = view.cursor_line(),
      .cursor_column = view.cursor_column(),
      .scroll_line = view.scroll_line(),
      .horizontal_scroll = view.horizontal_scroll(),
      .selection = view.selection_range(),
  };
  if (!editor.restore.collapsed_fold_openers.empty()) {
    handle.collapsed_fold_openers = editor.restore.collapsed_fold_openers;
  } else if (editor.folding_model != nullptr) {
    for (const editor::FoldRange& range : editor.folding_model->collapsed_ranges()) {
      handle.collapsed_fold_openers.push_back(range.opener_line);
    }
  }
  return handle;
}

inline std::string UntitledTitle(std::uint32_t untitled_number) {
  return untitled_number == 0 ? std::string("Untitled")
                              : "Untitled-" + std::to_string(untitled_number);
}

inline std::string TabTitleForPath(const std::filesystem::path& path, std::uint32_t untitled_number) {
  return path.empty() ? UntitledTitle(untitled_number) : path.filename().string();
}

inline std::uint64_t EnsureTabStableId(TabEntry& tab) {
  static std::uint64_t next_stable_id = 1;
  if (tab.stable_id == 0) {
    tab.stable_id = next_stable_id++;
  }
  return tab.stable_id;
}

[[nodiscard]] inline bool CompareLeftSideIsHeld(const CompareTabState& compare_tab) {
  return project::IsFullGitObjectId(compare_tab.commit_hash) && compare_tab.left_content &&
         compare_tab.build_options.left_exists.has_value();
}

[[nodiscard]] inline bool CompareRefreshReadsGit(const CompareTabState& compare_tab) {
  return !compare_tab.plain_compare &&
         (!CompareLeftSideIsHeld(compare_tab) || compare_tab.right_ref != "WORKTREE");
}

static_assert(std::is_default_constructible_v<TabEntry::EditorTabState>);

inline bool TabEntryIsDirty(const TabEntry& tab) {
  if (tab.kind == TabEntry::Kind::Compare && tab.compare.has_value()) {
    return tab.compare->right_editable && tab.compare->right_viewport.dirty();
  }
  if (tab.kind == TabEntry::Kind::Merge && tab.merge.has_value()) {
    return tab.merge->result_viewport.dirty();
  }
  if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
    return false;
  }
  return tab.editor_state->viewport.dirty();
}
static_assert(std::is_default_constructible_v<TabEntry::DeferredTabHandle>);

[[nodiscard]] inline const editor::TextViewport* LoadedBufferViewOf(const TabEntry& tab) {
  if (tab.kind == TabEntry::Kind::Editor && tab.editor_state.has_value() &&
      !tab.editor_state->content_pending()) {
    return &tab.editor_state->viewport;
  }
  if (tab.kind == TabEntry::Kind::Compare && tab.compare.has_value() && tab.compare->right_editable &&
      !tab.compare->load_pending) {
    return &tab.compare->right_viewport;
  }
  return nullptr;
}

struct TerminalLinkSpan {
  std::size_t row = 0;
  std::size_t begin_column = 0;
  std::size_t end_row = 0;
  std::size_t end_column = 0;
  friend bool operator==(const TerminalLinkSpan&, const TerminalLinkSpan&) = default;
};
struct TerminalLinkHit {
  std::string target;
  TerminalLinkSpan span;
  std::size_t pane = 0;
};

struct TerminalPaneState {
  terminal::TerminalSession session;
  std::optional<TerminalLinkSpan> hovered_link;
  terminal::TerminalLineRangeSnapshot visible_lines_snapshot;
  std::vector<std::uint64_t> visible_row_keys;
  terminal::TerminalCursorSnapshot frame_cursor{};
  std::size_t frame_line_count = 0;
  bool frame_reverse_video = false;
  std::optional<TerminalSelectionBounds> frame_selection;
  bool frame_selection_live = false;
  std::size_t visible_lines_first_row = 0;
  std::size_t visible_lines_max_rows = 0;
  int scroll_row = 0;
  bool follow_tail = true;
  bool focus_events_active = false;
  bool mouse_selecting = false;
  std::optional<TerminalSelectionPoint> selection_anchor;
  std::optional<TerminalSelectionPoint> selection_head;
  TerminalSelectionUnit selection_unit = TerminalSelectionUnit::Cell;
  TerminalSelectionBounds selection_origin{};
  static constexpr std::size_t kMaxPendingInputBytes = 1u * 1024 * 1024;
  std::string pending_input;
  bool pending_input_truncated = false;
  std::string last_command_invocation;
  std::string last_command_prompt_prefix;
  std::size_t last_command_start_row = 0;
  bool has_last_command = false;
  std::uint64_t observed_scrollback_trim_total = 0;
  std::uint64_t observed_row_shift_sequence = 0;
  bool observed_alternate_screen = false;
  std::filesystem::path launch_working_directory;
  std::string launch_command;
  std::shared_ptr<const platform::ProcessLauncher> launcher;
  std::string label_prefix;
  bool reattach_unreported = false;
  std::uint64_t seen_host_handle = 0;
};

struct TerminalTabState {
  std::vector<std::unique_ptr<TerminalPaneState>> panes;
  TerminalPaneWeights weights;
  std::size_t active_pane = 0;
  bool has_unseen_output = false;
  bool has_unseen_bell = false;
  std::uint64_t layout_revision = 0;

  TerminalPaneState* active() {
    return active_pane < panes.size() ? panes[active_pane].get() : nullptr;
  }
  const TerminalPaneState* active() const {
    return active_pane < panes.size() ? panes[active_pane].get() : nullptr;
  }
  std::size_t pane_count() const { return panes.size(); }
  bool full() const { return panes.size() >= kMaxTerminalPanes; }

  bool InsertPane(std::size_t index, std::unique_ptr<TerminalPaneState> pane) {
    if (full() || pane == nullptr) {
      return false;
    }
    index = std::min(index, panes.size());
    InsertTerminalPaneWeight(weights, index);
    panes.insert(panes.begin() + static_cast<std::ptrdiff_t>(index), std::move(pane));
    active_pane = index;
    ++layout_revision;
    return true;
  }

  std::unique_ptr<TerminalPaneState> RemovePane(std::size_t index) {
    if (index >= panes.size()) {
      return nullptr;
    }
    std::unique_ptr<TerminalPaneState> removed = std::move(panes[index]);
    panes.erase(panes.begin() + static_cast<std::ptrdiff_t>(index));
    RemoveTerminalPaneWeight(weights, index);
    if (panes.empty()) {
      active_pane = 0;
    } else if (active_pane > index || active_pane >= panes.size()) {
      active_pane = std::min(active_pane == 0 ? 0 : active_pane - 1, panes.size() - 1);
    }
    ++layout_revision;
    return removed;
  }

  bool ResizeDivider(std::size_t boundary, float first_share) {
    if (!ResizeTerminalPaneDivider(weights, boundary, first_share)) {
      return false;
    }
    ++layout_revision;
    return true;
  }
  bool ResetDivider(std::size_t boundary) { return ResizeDivider(boundary, 0.5f); }

  TerminalPaneRectsLayout PaneRects(const SDL_FRect& body) const {
    return ComputeTerminalPaneRects(body, std::span<const float>(weights.data(), weights.size()));
  }
  SDL_FRect ActivePaneRect(const SDL_FRect& body) const {
    if (panes.size() < 2) {
      return body;
    }
    const TerminalPaneRectsLayout rects = PaneRects(body);
    return active_pane < rects.panes.size() ? rects.panes[active_pane] : body;
  }
};

inline std::unique_ptr<TerminalTabState> MakeTerminalTab(std::unique_ptr<TerminalPaneState> pane) {
  auto tab = std::make_unique<TerminalTabState>();
  tab->InsertPane(0, std::move(pane));
  return tab;
}

struct EditorPreferences {
  std::size_t tab_size = 4;
  std::size_t indent_width = 4;
  bool soft_tabs = true;
  bool soft_wrap = false;
  int font_size = 13;
};

[[nodiscard]] inline const std::filesystem::path& EditorViewPathRef(
    const TabEntry::EditorTabState& editor_state) {
  return editor_state.content_pending() ? editor_state.restore.path
                                        : editor_state.viewport.path();
}

[[nodiscard]] inline bool EditorViewPathIs(const TabEntry::EditorTabState& editor_state,
                                           const std::filesystem::path& normalized_path) {
  return util::SameAsNormalizedPath(EditorViewPathRef(editor_state), normalized_path);
}

}
