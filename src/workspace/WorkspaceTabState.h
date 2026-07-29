#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "compare/CompareModel.h"
#include "compare/ComparePresentationModel.h"
#include "compare/CompareReviewTypes.h"
#include "compare/CompareSemanticMetadata.h"
#include "compare/MergeConflictKind.h"
#include "compare/MergeModel.h"
#include "editor/FoldingModel.h"
#include "editor/SnippetEngine.h"
#include "editor/TextLayout.h"
#include "editor/TextViewport.h"
#include "terminal/TerminalSession.h"
#include "workspace/OverviewRuler.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/WorkspaceTerminalSelection.h"

namespace microide::workspace {

enum class EditorSplitOrientation {
  None,
  Vertical,
  Horizontal,
};

enum class CompareHoverKind {
  CollapsedContextPreviousAction,
  CollapsedContextAllAction,
  CollapsedContextNextAction,
};

struct CompareHoverState {
  CompareHoverKind kind = CompareHoverKind::CollapsedContextAllAction;
  std::size_t presentation_row = 0;
  std::size_t collapsed_run_start_model_row = 0;
  std::size_t collapsed_run_length = 0;
  bool operator==(const CompareHoverState&) const = default;
};

struct CompareReviewHeaderState {
  std::string summary_line;
  std::string action_hint_line;
};

struct CompareVisibleLayoutCacheEntry {
  std::size_t model_row = 0;
  std::size_t horizontal_scroll = 0;
  std::size_t visible_columns = 0;
  std::size_t tab_size = 0;
  bool right_side = false;
  editor::LayoutLine layout;
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

struct CompareTabState {
  std::filesystem::path path;
  std::filesystem::path left_path;
  std::filesystem::path right_path;
  std::string title;
  std::string commit_hash;
  std::string right_ref;
  std::string left_label;
  std::string right_label;
  std::string left_content;
  compare::CompareReviewMode review_mode = compare::CompareReviewMode::WorkingTree;
  compare::WorkingTreeStagingView staging_view = compare::WorkingTreeStagingView::Combined;
  compare::BranchReviewTargetIdentity branch_target;
  compare::CompareSemanticFileMetadata semantic_file;
  compare::ComparePresentationModel presentation;
  CompareReviewHeaderState review_header;
  std::uint64_t presentation_revision = 0;
  compare::CompareBuildOptions build_options;
  bool show_whitespace = false;
  bool opened_from_commit_picker = false;
  std::vector<std::filesystem::path> review_files;
  std::size_t review_file_index = 0;
  editor::SyntaxState left_initial_syntax_state;
  editor::SyntaxState right_initial_syntax_state;
  editor::SyntaxState left_current_syntax_state;
  editor::SyntaxState right_current_syntax_state;
  compare::CompareModel model;
  editor::TextViewport right_viewport;
  std::vector<std::vector<editor::SyntaxTokenKind>> left_tokens_by_row;
  std::vector<std::vector<editor::SyntaxTokenKind>> right_tokens_by_row;
  std::uint64_t visible_layout_cache_model_revision = 0;
  std::vector<CompareVisibleLayoutCacheEntry> visible_layout_cache;
  std::unordered_map<CompareVisibleLayoutCacheKey, std::size_t,
                     CompareVisibleLayoutCacheKeyHash>
      visible_layout_cache_index;
  std::size_t syntax_rows_tokenized = 0;
  bool syntax_highlighting_enabled = true;
  std::uint64_t model_revision = 0;
  // Cheap change-detection signals the model + syntax + tokenization were last built
  // from. The editable right pane is the one refreshes touch repeatedly (every keystroke,
  // mouse, focus, plugin, external-change event fires RefreshCompareTabDerivedState from
  // ~10 sites, most leaving content untouched), so its change is detected by the viewport's
  // monotonic `content_revision` + line ending — no whole-buffer serialize on the no-op
  // path. The read-only `left_content` string has no viewport/revision, so it is hashed
  // directly; it is write-once in production (BuildCompareTabFromBuffers on a fresh state)
  // and the hash is allocation-free. Together with the ignore-whitespace option these
  // reproduce the old fingerprint's coverage without the per-refresh right-buffer copy.
  std::uint64_t derived_right_content_revision = 0;
  util::LineEnding derived_right_line_ending = util::LineEnding::LF;
  std::size_t derived_left_content_hash = 0;
  // Cached line count of the read-only left content, so compare gutter sizing does not
  // rescan left_content for '\n' on every render/hit-test/scroll/cursor layout request
  // (TD-2026-07-17A-094). Recomputed only when the derived fingerprint rebuilds.
  std::size_t derived_left_line_count = 0;
  bool derived_ignore_whitespace = false;
  bool derived_fingerprint_valid = false;
  bool model_stale = false;
  bool model_refreshing = false;
  std::optional<CompareHoverState> hover_state;
  std::size_t selected_row = 0;
  int scroll_row = 0;
  std::size_t horizontal_scroll = 0;
  std::size_t max_visual_columns = 0;
  bool scrollbar_marker_cache_valid = false;
  std::uint64_t scrollbar_marker_cache_revision = 0;
  std::uint64_t scrollbar_marker_cache_theme_token = 0;
  SDL_FRect scrollbar_marker_cache_track{};
  std::vector<overview::Marker> scrollbar_marker_cache;
  float divider_fraction = kWorkspaceDefaultCompareDividerFraction;
  bool right_editable = false;
  bool right_view_active = false;
  bool persistable = true;
  // Sticky flag marking a non-git ("plain") comparison — two arbitrary sides
  // (file/buffer/clipboard) with no repository backing. When set, the derived-
  // state refresh forces review_mode to Plain instead of re-inferring it from
  // git refs, keeping staging/branch-review off. See ApplyCompareTabReviewMetadata.
  bool plain_compare = false;
};

struct MergeTabState {
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
  std::string status_message;
  editor::TextViewport::LineEnding result_line_ending = editor::TextViewport::LineEnding::LF;
  compare::MergeModel model;
  std::vector<std::vector<editor::SyntaxTokenKind>> incoming_tokens;
  std::vector<std::vector<editor::SyntaxTokenKind>> current_tokens;
  editor::SyntaxState incoming_initial_syntax_state;
  editor::SyntaxState incoming_current_syntax_state;
  editor::SyntaxState current_initial_syntax_state;
  editor::SyntaxState current_current_syntax_state;
  std::size_t incoming_syntax_rows_tokenized = 0;
  std::size_t current_syntax_rows_tokenized = 0;
  editor::TextViewport result_viewport;
  std::optional<std::string> persisted_output_baseline;
  std::vector<MergeTrackedConflict> conflicts;
  std::optional<MergeHoverState> hover_state;
  std::size_t selected_hunk = 0;
  int scroll_row = 0;
  std::size_t horizontal_scroll = 0;
  std::size_t max_visual_columns = 0;
  std::uint64_t model_revision = 0;
  bool scrollbar_marker_cache_valid = false;
  std::uint64_t scrollbar_marker_cache_revision = 0;
  std::uint64_t scrollbar_marker_cache_theme_token = 0;
  SDL_FRect scrollbar_marker_cache_track{};
  std::vector<overview::Marker> scrollbar_marker_cache;
  // Cache for the hover-preview overlay's choice lines (MergeChoiceLines), keyed
  // by (conflict, choice, model revision). Rebuilt only on a key change so hover
  // does not reallocate the incoming/current line vectors every frame.
  std::vector<std::string> preview_lines_cache;
  bool preview_lines_cache_valid = false;
  std::size_t preview_lines_cache_conflict = 0;
  compare::MergeChoice preview_lines_cache_choice = compare::MergeChoice::Base;
  std::uint64_t preview_lines_cache_revision = 0;
  float left_divider_fraction = kWorkspaceDefaultMergeLeftDividerFraction;
  float right_divider_fraction = kWorkspaceDefaultMergeRightDividerFraction;
  bool persistable = true;
};

struct TabEntry {
  enum class Kind {
    Editor,
    Compare,
    Merge,
  };

  struct EditorTabState {
    // A tab owns exactly one editor viewport. Side-by-side / stacked layouts are
    // modelled as editor *groups* above the tab level (see `EditorGroup`), not as
    // a split tree inside a tab.
    editor::TextViewport viewport;
    // Deferred-restore metadata: while `needs_restore` is true the viewport is a
    // placeholder and these fields carry the real on-disk path + caret/scroll so
    // the tab can be hydrated lazily (session restore / background open).
    std::filesystem::path restored_path;
    std::size_t restored_cursor_line = 0;
    std::size_t restored_cursor_column = 0;
    std::size_t restored_scroll_line = 0;
    std::size_t restored_horizontal_scroll = 0;
    bool needs_restore = false;
    // Per-tab fold-region model. Lazily computed by the renderer / fold action
    // path through `EnsureFoldingModelFresh(...)`. Cleared automatically on tab
    // close; rekeyed implicitly through its `(layout_revision, tab_size,
    // language_id)` fingerprint when the buffer or language changes.
    std::unique_ptr<editor::FoldingModel> folding_model =
        std::make_unique<editor::FoldingModel>();
    editor::SnippetSessionState snippet_session;
  };

  struct DeferredTabHandle {
    std::filesystem::path path;
    std::string language_hint;
    std::size_t cursor_line = 0;
    std::size_t cursor_column = 0;
    std::size_t scroll_line = 0;
    std::size_t horizontal_scroll = 0;
    std::optional<editor::SelectionRange> selection;
  };

  Kind kind = Kind::Editor;
  std::filesystem::path path;
  std::string title;
  // Stable per-tab identity, assigned lazily (from ProjectWorkspaceState::
  // next_tab_stable_id) the first time a tab is referenced by a modal dirty prompt.
  // 0 = unassigned. Lets a dirty prompt survive a tab close/reorder while it is up:
  // the prompt stores ids, not indices, and resolves them back to current indices at
  // confirm time — so it never saves/closes the wrong tab (TD-2026-07-17-024). Purely
  // in-memory (a modal prompt never survives a session save), so it is not persisted.
  std::uint64_t stable_id = 0;
  std::optional<EditorTabState> editor_state;
  std::optional<DeferredTabHandle> deferred_handle;
  std::optional<CompareTabState> compare;
  std::optional<MergeTabState> merge;
};

struct TerminalTabState {
  terminal::TerminalSession session;
  terminal::TerminalLineRangeSnapshot visible_lines_snapshot;
  std::size_t visible_lines_first_row = 0;
  std::size_t visible_lines_max_rows = 0;
  int scroll_row = 0;
  bool follow_tail = true;
  bool focus_events_active = false;
  bool mouse_selecting = false;
  std::optional<TerminalSelectionPoint> selection_anchor;
  std::optional<TerminalSelectionPoint> selection_head;
  // Host-side capture of bytes typed/pasted since the last Enter, used ONLY to strip
  // the prompt prefix from the copy-last-command transcript (never sent to the PTY).
  // Bounded by kMaxPendingInputBytes: repeated pastes/programmatic input before a
  // newline must not grow this without limit. Once the budget is hit, further bytes
  // are dropped and `pending_input_truncated` is set so submit skips the (now
  // unreliable) prefix match instead of using a partial capture. TD-2026-07-17A-068.
  static constexpr std::size_t kMaxPendingInputBytes = 1u * 1024 * 1024;  // 1 MiB
  std::string pending_input;
  bool pending_input_truncated = false;
  std::string last_command_invocation;
  std::string last_command_prompt_prefix;
  std::size_t last_command_start_row = 0;
  bool has_last_command = false;
  // Last-observed value of session.ScrollbackTrimTotal(); the delta rebases the
  // absolute-row mirrors below (scroll_row, selection, last_command_start_row) when
  // scrollback is trimmed, so they track the same content instead of jumping.
  std::uint64_t observed_scrollback_trim_total = 0;
};

struct EditorPreferences {
  std::size_t tab_size = 4;
  std::size_t indent_width = 4;
  bool soft_tabs = false;
  bool soft_wrap = false;
  // Editor glyph point size for this project (the `editor.font_size` setting).
  // Applied to the shared text renderer when the project's preferences are
  // applied; clamped to the setting range (8..32) on load/set.
  int font_size = 13;
};

}  // namespace microide::workspace
