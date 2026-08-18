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
#include "compare/CompareModel.h"
#include "compare/ComparePresentationModel.h"
#include "compare/CompareReviewTypes.h"
#include "compare/CompareSemanticMetadata.h"
#include "compare/MergeConflictKind.h"
#include "compare/MergeModel.h"
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
#include "workspace/WorkspaceLayout.h"
#include "workspace/WorkspaceTerminalSelection.h"

namespace microide::workspace {

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

// Visible-row layouts for the two diff panes, and the index that finds them.
//
// Both halves are STEADY-STATE ALLOCATION-FREE on purpose, because both used to
// be rebuilt from nothing on every keystroke: a keystroke in the editable pane
// bumps `model_revision`, and the invalidation that follows dropped the whole
// window (TD-2026-08-17-261).
//
//  - `layouts` is a SLAB, not a live list. Only `[0, live_count)` is addressed
//    by the index; everything past that is retained storage whose three heap
//    buffers the next build refills in place through
//    `TextLayout::BuildVisibleLineInto`. Freeing them cost three allocations per
//    visible row per keystroke — the top three sites of the phase.
//  - `table` is an open-addressed index into the slab holding `slot + 1`, 0 for
//    empty, sized so the load factor never exceeds 1/2 (so linear probing always
//    terminates). It replaced a `std::unordered_map`, which cost one NODE
//    allocation per cached row per frame: `clear()` frees every node and the
//    next frame's inserts allocate them straight back. Resetting this refills
//    4 KB with zero and allocates nothing.
//  - `keys[i]` describes `layouts[i]`, so a probe can confirm a hit.
//
// Reset it with `ResetCompareVisibleLayoutCache` in
// `workspace/render/CompareVisibleLayoutCache.h`; nothing should clear these
// vectors.
struct CompareVisibleLayoutCache {
  // Bounded by the table's half-load rule below; see kCompareVisibleLayoutTableSize.
  static constexpr std::size_t kLimit = 512;
  static constexpr std::size_t kTableSize = 1024;  // power of two, >= 2 * kLimit

  std::uint64_t model_revision = 0;
  std::size_t live_count = 0;
  std::vector<editor::LayoutLine> layouts;
  std::vector<CompareVisibleLayoutCacheKey> keys;
  std::vector<std::uint32_t> table;
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
  // The read-only left side, shared with `model.left_source` rather than copied
  // into it. The left buffer never changes for the tab's life, and a rebuild runs
  // on every keystroke in the editable right pane, so an owned std::string here
  // meant memcpy'ing the whole left file per keystroke — and holding two resident
  // copies of it (TD-2026-08-14-232). Never null; empty text is a shared singleton.
  compare::CompareTextBuffer left_content = compare::EmptyCompareText();
  compare::CompareReviewMode review_mode = compare::CompareReviewMode::WorkingTree;
  compare::WorkingTreeStagingView staging_view = compare::WorkingTreeStagingView::Combined;
  compare::BranchReviewTargetIdentity branch_target;
  compare::CompareSemanticFileMetadata semantic_file;
  compare::ComparePresentationModel presentation;
  CompareReviewHeaderState review_header;
  // Scratch for the whole-file review-marker resolve, kept on the tab so its
  // buffer survives across passes instead of being reallocated per refresh.
  std::vector<compare::BranchReviewHunkMarker> review_hunk_markers;
  // What the rows' review markers were last composed from. The marker pass is
  // O(presentation rows) and runs from the derived-state refresh, which fires on
  // every mouse move; these let it skip the events that moved none of its inputs.
  compare::BranchReviewTargetIdentity review_markers_built_target;
  std::uint64_t review_markers_built_presentation_revision = 0;
  std::uint64_t review_markers_built_review_revision = 0;
  bool review_markers_valid = false;
  std::uint64_t presentation_revision = 0;
  compare::CompareBuildOptions build_options;
  bool show_whitespace = false;
  bool opened_from_commit_picker = false;
  std::vector<std::filesystem::path> review_files;
  std::size_t review_file_index = 0;
  compare::CompareModel model;
  editor::TextViewport right_viewport;
  // Soft-wrap row table for the two panes. Inactive (and empty) unless
  // `editor.wrap` is on, in which case a presentation row occupies
  // max(left segments, right segments) on-screen rows and the shorter side is
  // padded with blank rows so the panes stay aligned (TD-2026-08-13-200).
  // `scroll_row` is an index into THIS table; `selected_row` stays a
  // presentation-row index, so a selected wrapped line highlights whole.
  DiffWrapLayout wrap_layout;
  // Syntax tokens for the two diff panes, indexed by MODEL row. See
  // SurfaceTokenWindow: these used to be a `vector<vector<SyntaxTokenKind>>`
  // holding one heap buffer per row of the diff, filled by a monotone frontier
  // and never released — 86 % of `diff.next_hunk_burst`'s allocations.
  //
  // The alias flag that used to sit beside them is gone with it. It existed
  // because an unchanged row whose two sides are byte-identical highlights to the
  // same token run, which the old cache stored twice, once per pane, for every
  // such row in the FILE (TD-2026-08-06-159). What the window bounds is exactly
  // that: only the rows on screen hold tokens at all, so the second copy costs a
  // pooled buffer and one tokenize of a line already in cache, and the flag array
  // plus its two read sites cost more than they save.
  SurfaceTokenWindow left_token_window;
  SurfaceTokenWindow right_token_window;
  // Bracket-match memo for the editable pane, keyed exactly as the editor
  // renderer's is: (content revision, caret line, caret column). FindBracketMatch
  // is O(file), and the compare surface paints through its own row loop with no
  // access to the editor renderer's cache — so without this, turning the
  // highlight on would have traded a per-frame O(file) scan for it
  // (TD-2026-08-13-206). One entry, which is the natural cardinality: a tab has
  // one caret.
  std::uint64_t bracket_match_content_revision = 0;
  std::size_t bracket_match_caret_line = 0;
  std::size_t bracket_match_caret_column = 0;
  std::optional<editor::BracketMatchPair> bracket_match_pair;
  bool bracket_match_valid = false;

  CompareVisibleLayoutCache visible_layouts;
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
  // Semantic classification (binary / submodule / line-ending-only) and the
  // presentation model derive from the same two content buffers the fingerprint
  // above guards, so they are gated on it too. Recomputing them per refresh cost a
  // whole-buffer serialize of the right pane plus several full scans of both files
  // on every keystroke, mouse, focus, and plugin event — the exact work the
  // fingerprint exists to skip. `semantic_inputs_valid` additionally covers the git
  // entry the classifier reads (rename/mode), which the model fingerprint does not.
  bool semantic_inputs_valid = false;
  std::size_t semantic_git_entry_signature = 0;
  // Guards the presentation rebuild. Bumped whenever anything the builder consumes
  // that is NOT the model changes: the semantic metadata and the whitespace option.
  // Collapse-state mutations refresh the presentation directly through
  // RefreshCompareTabPresentation, which revalidates this.
  bool presentation_valid = false;
  std::uint64_t presentation_built_model_revision = 0;
  bool presentation_built_show_whitespace = false;
  bool model_stale = false;
  bool model_refreshing = false;
  std::optional<CompareHoverState> hover_state;
  std::size_t selected_row = 0;
  int scroll_row = 0;
  std::size_t horizontal_scroll = 0;
  std::size_t max_visual_columns = 0;
  bool scrollbar_marker_cache_valid = false;
  std::uint64_t scrollbar_marker_cache_revision = 0;
  // On-screen row count the cached markers were scaled against. Soft wrap makes
  // that a different number from the presentation-row count and moves it on every
  // re-wrap (a divider drag, a window resize), so it is part of the key.
  std::size_t scrollbar_marker_cache_rows = 0;
  std::uint64_t scrollbar_marker_cache_theme_token = 0;
  SDL_FRect scrollbar_marker_cache_track{};
  std::vector<overview::Marker> scrollbar_marker_cache;
  // Working buffers for the marker rebuild above, retained because the cache key
  // includes `presentation_revision` — so a keystroke in the editable pane
  // invalidates it and the rebuild ran with three fresh vectors per keystroke
  // (TD-2026-08-17-261). Meaningful only inside EnsureCompareOverviewMarkers.
  std::vector<CompareScrollbarRun> scrollbar_run_scratch;
  std::vector<overview::MarkerInput> scrollbar_marker_input_scratch;
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

// True when this model row's right-pane tokens ARE its left-pane tokens: both
// sides present, byte-identical, and resuming from the same syntax state, which
// makes the two panes produce the same token run.
//
// A diff is mostly unchanged rows, so this is most of them. The right window
// declines to tokenize such a row and the render site reads the left window's
// buffer, which saves both the second tokenize and the second buffer.
//
// ONE definition, called from the tokenizer and from the render site, because
// the two must agree exactly: if the render reads left's buffer for a row the
// tokenizer did fill on the right, or vice versa, the pane paints unstyled.
inline bool CompareRowRightTokensAliasLeft(const CompareTabState& tab, std::size_t row) {
  if (row >= tab.model.rows.size()) {
    return false;
  }
  const compare::CompareRow& compare_row = tab.model.rows[row];
  return compare_row.kind == compare::CompareRowKind::Unchanged && compare_row.left_line > 0 &&
         compare_row.right_line > 0 && compare_row.left_text == compare_row.right_text &&
         tab.left_token_window.StateBefore(row) == tab.right_token_window.StateBefore(row);
}

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
  // Syntax tokens for the two read-only source panes. See SurfaceTokenWindow:
  // these used to be a `vector<vector<SyntaxTokenKind>>` holding one heap buffer
  // per LINE OF THE FILE, filled by a monotone frontier and never released.
  SurfaceTokenWindow incoming_token_window;
  SurfaceTokenWindow current_token_window;
  editor::TextViewport result_viewport;
  // Soft-wrap row table for the two read-only source panes (incoming = left,
  // current = right). Inactive unless `editor.wrap` is on. The result pane wraps
  // through its own viewport; the three panes share `scroll_row`, which is a
  // visual-row index in every mode.
  DiffWrapLayout wrap_layout;
  std::optional<std::string> persisted_output_baseline;
  std::vector<MergeTrackedConflict> conflicts;
  // `conflicts` with every line field projected into on-screen row space (see
  // workspace/MergeWrapRows.h). Derived cache, warmed from const geometry paths;
  // unused (and empty) while wrap is off, where the conflicts already ARE rows.
  mutable std::vector<MergeTrackedConflict> visual_conflicts;
  mutable std::uint64_t visual_conflicts_key = 0;
  mutable bool visual_conflicts_valid = false;
  std::optional<MergeHoverState> hover_state;
  std::size_t selected_hunk = 0;
  int scroll_row = 0;
  std::size_t horizontal_scroll = 0;
  std::size_t max_visual_columns = 0;
  std::uint64_t model_revision = 0;
  bool scrollbar_marker_cache_valid = false;
  std::uint64_t scrollbar_marker_cache_revision = 0;
  // See the compare tab's field of the same name.
  std::size_t scrollbar_marker_cache_rows = 0;
  std::uint64_t scrollbar_marker_cache_theme_token = 0;
  SDL_FRect scrollbar_marker_cache_track{};
  std::vector<overview::Marker> scrollbar_marker_cache;
  // See the compare tab's field of the same name: the marker rebuild's working
  // buffer, retained so an invalidation does not re-allocate it.
  std::vector<overview::MarkerInput> scrollbar_marker_input_scratch;
  // Cache for the hover-preview overlay's choice lines (MergeChoiceLines), keyed
  // by (conflict, choice, model revision). Rebuilt only on a key change so hover
  // does not reallocate the incoming/current line vectors every frame.
  std::vector<std::string> preview_lines_cache;
  bool preview_lines_cache_valid = false;
  std::size_t preview_lines_cache_conflict = 0;
  compare::MergeChoice preview_lines_cache_choice = compare::MergeChoice::Base;
  std::uint64_t preview_lines_cache_revision = 0;
  // Reused buffer for the toolbar's "Conflict 2/7 | remaining 5 | dirty" line,
  // which the merge surface rebuilds on every painted frame. See
  // `BuildMergeResolverStatus` — the status it returns views this.
  std::string resolver_progress_buffer;
  float left_divider_fraction = kWorkspaceDefaultMergeLeftDividerFraction;
  float right_divider_fraction = kWorkspaceDefaultMergeRightDividerFraction;
  bool persistable = true;
};

// Namespace scope, NOT nested in `TabEntry`, and it must stay that way: a class
// with a default member initializer that is nested inside the class holding an
// `std::optional` of it is not `is_constructible` at the point the optional is
// declared (the NSDMI is parsed only at the closing brace of the *enclosing*
// class). GCC re-evaluates the trait later; clang caches the `false` for the
// whole translation unit, so `optional<...>::emplace()` then fails to compile
// under clang and only under clang. TD-2026-08-14-214. `TabEntry` keeps the
// `TabEntry::EditorTabState` spelling as an alias below.
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

// Namespace scope for the same reason as `EditorTabState` above: it has no NSDMI
// today, but a nested type reachable through an `optional` member of its own
// enclosing class is the landmine, not the initializer.
struct DeferredTabHandle {
  std::filesystem::path path;
  std::string language_hint;
  std::size_t cursor_line = 0;
  std::size_t cursor_column = 0;
  std::size_t scroll_line = 0;
  std::size_t horizontal_scroll = 0;
  std::optional<editor::SelectionRange> selection;
};

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

// The trait clang caches as `false` when the optional's element type is nested in
// the class holding the optional (TD-2026-08-14-214). Asserting it here — after
// `TabEntry` is complete — makes the landmine a compile error in the header that
// owns the shape, rather than an error at whichever call site next reaches for
// `emplace()`.
static_assert(std::is_default_constructible_v<TabEntry::EditorTabState>);
static_assert(std::is_default_constructible_v<TabEntry::DeferredTabHandle>);

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

// The path an editor tab's view is showing, without materializing it.
//
// A deferred-restore tab has not opened its viewport yet, so its identity lives
// in `restored_path`; every other tab's is the viewport's own. Both are stored
// normalized, which is why this can hand back a reference: the callers that need
// a normalized `std::filesystem::path` value used to build one per tab per scan,
// and a path copy is roughly as expensive as `lexically_normal()` (a fresh
// pathname string plus a component list with a path per component).
[[nodiscard]] inline const std::filesystem::path& EditorViewPathRef(
    const TabEntry::EditorTabState& editor_state) {
  return editor_state.needs_restore ? editor_state.restored_path
                                    : editor_state.viewport.path();
}

// Whether an editor tab's view is showing `normalized_path`, which the caller
// must already have in lexically-normal form.
//
// This used to carry its own byte-identical copy of `util::SameAsNormalizedPath`'s
// two guards (TD-2026-08-06-159), written before that helper existed. The reason
// they matter lives with the helper now: a scan over open tabs is mostly
// mismatches, and normalizing each candidate to reject it is ~12 allocations
// spent to learn nothing.
[[nodiscard]] inline bool EditorViewPathIs(const TabEntry::EditorTabState& editor_state,
                                           const std::filesystem::path& normalized_path) {
  return util::SameAsNormalizedPath(EditorViewPathRef(editor_state), normalized_path);
}

}  // namespace microide::workspace
