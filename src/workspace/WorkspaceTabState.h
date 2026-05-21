#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "compare/CompareModel.h"
#include "compare/MergeModel.h"
#include "editor/FoldingModel.h"
#include "editor/SnippetEngine.h"
#include "editor/TextViewport.h"
#include "terminal/TerminalSession.h"
#include "workspace/WorkspaceLayout.h"

namespace microide::workspace {

enum class EditorSplitOrientation {
  None,
  Vertical,
  Horizontal,
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
  editor::SyntaxState left_initial_syntax_state;
  editor::SyntaxState right_initial_syntax_state;
  editor::SyntaxState left_current_syntax_state;
  editor::SyntaxState right_current_syntax_state;
  compare::CompareModel model;
  editor::TextViewport right_viewport;
  std::vector<std::vector<editor::SyntaxTokenKind>> left_tokens_by_row;
  std::vector<std::vector<editor::SyntaxTokenKind>> right_tokens_by_row;
  std::size_t syntax_rows_tokenized = 0;
  bool syntax_highlighting_enabled = true;
  std::uint64_t model_revision = 0;
  std::size_t selected_row = 0;
  int scroll_row = 0;
  std::size_t horizontal_scroll = 0;
  std::size_t max_visual_columns = 0;
  bool scrollbar_marker_cache_valid = false;
  std::uint64_t scrollbar_marker_cache_revision = 0;
  SDL_FRect scrollbar_marker_cache_track{};
  std::vector<CompareScrollbarMarker> scrollbar_marker_cache;
  float divider_fraction = 0.5f;
  bool right_editable = false;
  bool right_view_active = false;
  bool persistable = true;
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
  SDL_FRect scrollbar_marker_cache_track{};
  std::vector<MergeScrollbarMarker> scrollbar_marker_cache;
  float left_divider_fraction = 1.0f / 3.0f;
  float right_divider_fraction = 2.0f / 3.0f;
  bool persistable = true;
};

struct TabEntry {
  enum class Kind {
    Editor,
    Compare,
    Merge,
  };

  struct EditorTabState {
    struct EditorViewState {
      std::size_t leaf_id = 0;
      editor::TextViewport viewport;
      std::filesystem::path restored_path;
      std::size_t restored_cursor_line = 0;
      std::size_t restored_cursor_column = 0;
      std::size_t restored_scroll_line = 0;
      std::size_t restored_horizontal_scroll = 0;
      bool needs_restore = false;
    };

    struct EditorSplitNode {
      std::size_t leaf_id = 0;
      EditorSplitOrientation orientation = EditorSplitOrientation::None;
      float size_fraction = 1.0f;
      std::vector<std::unique_ptr<EditorSplitNode>> children;

      bool IsLeaf() const { return children.empty(); }
    };

    std::vector<EditorViewState> views;
    std::size_t active_leaf_id = 0;
    std::size_t next_leaf_id = 1;
    std::unique_ptr<EditorSplitNode> split_root;
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
    std::size_t active_leaf_id = 1;
  };

  Kind kind = Kind::Editor;
  std::filesystem::path path;
  std::string title;
  std::optional<EditorTabState> editor_state;
  std::optional<DeferredTabHandle> deferred_handle;
  std::optional<CompareTabState> compare;
  std::optional<MergeTabState> merge;
};

struct TerminalSelectionPosition {
  std::size_t row = 0;
  std::size_t column = 0;
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
  std::optional<TerminalSelectionPosition> selection_anchor;
  std::optional<TerminalSelectionPosition> selection_head;
  std::string pending_input;
  std::string last_command_invocation;
  std::string last_command_prompt_prefix;
  std::size_t last_command_start_row = 0;
  bool has_last_command = false;
};

struct EditorPreferences {
  std::size_t tab_size = 4;
  std::size_t indent_width = 4;
  bool soft_tabs = false;
  bool soft_wrap = false;
};

}  // namespace microide::workspace
