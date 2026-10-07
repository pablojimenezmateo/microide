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
  // path. The read-only left side is an IMMUTABLE shared buffer, so its identity is its
  // content: a new left side is always a new buffer. It used to be hashed instead, which
  // is allocation-free but O(file) — on every refresh, and the refresh fires on every
  // mouse move, so a 200 MB left side cost a full pass over 200 MB per pointer motion.
  // The fingerprint HOLDS the buffer rather than its address: a raw address could be
  // freed and reused by a later buffer and then compare equal to it.
  std::uint64_t derived_right_content_revision = 0;
  util::LineEnding derived_right_line_ending = util::LineEnding::LF;
  compare::CompareTextBuffer derived_left_content;
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
  // A tab created before its content (TD-2026-09-29-312): a large working-tree
  // compare opens as an empty, read-only stand-in and loads off the shell thread.
  // `load_pending` means "this tab still owes a load"; the frame that SHOWS it
  // posts the load and arms `pending_load`. Lazy on purpose, as VS Code's diff
  // editor is: a review that opens thirty large files reads the one on screen,
  // and a load cancelled by a project switch simply runs again the next time the
  // tab is shown, because cancelling disarms the slot and leaves this set.
  bool load_pending = false;
  editor::AsyncBufferWork pending_load;
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
  // Where this tab's content is. A tab can exist before its content does — a
  // session-restored tab that has never been activated, and a file large enough
  // that reading it on the shell thread would drop frames — and every guard in
  // the tree asks the same question of it: is the viewport the file yet?
  //
  // `Deferred` is the lazily-hydrated tab: nothing has been read, and the
  // `restored_*` fields below carry the on-disk path plus the caret and scroll
  // to place once it is. `Loading` is the same tab with a read already in
  // flight (`pending_load` names it). `Failed` is a read that finished and
  // could not produce a buffer; the tab stays, so the user sees which file it
  // was rather than a tab that silently vanished.
  enum class Content : std::uint8_t {
    Ready,
    Deferred,
    Loading,
    Failed,
  };
  Content content = Content::Ready;
  // The one question almost every call site actually asks: is the viewport NOT
  // the file? Every non-Ready state answers yes, so a new state cannot be
  // forgotten at a site that only knew about deferred restore.
  [[nodiscard]] bool content_pending() const { return content != Content::Ready; }

  // Deferred-restore metadata: while `content_pending()` the viewport is empty
  // and these fields carry the real on-disk path + caret/scroll so the tab can
  // be hydrated lazily (session restore / background open).
  std::filesystem::path restored_path;
  // `reveal` targeted this tab while its content was still loading: centre the
  // restored caret line once the load lands (the placeholder had no real size).
  bool center_cursor_on_load = false;
  std::size_t restored_cursor_line = 0;
  std::size_t restored_cursor_column = 0;
  std::size_t restored_scroll_line = 0;
  std::size_t restored_horizontal_scroll = 0;

  // Format-on-save runs off the shell thread, so a save can be in flight for this
  // tab with nothing written yet. Armed while that is true, with the
  // SaveFormatterService run id, so the completion finds its tab by matching it
  // and drops the formatter's output if the buffer moved on under it —
  // reformatting a buffer the user has since typed into would undo their edit.
  editor::AsyncBufferWork pending_format_save;
  // Armed while `content == Loading`: the off-thread read filling this tab. The
  // completion finds its tab by matching this id, so a tab closed mid-read
  // simply has no match and the bytes are dropped.
  editor::AsyncBufferWork pending_load;
  // One-shot, set by that completion: the save it re-enters must not start another
  // formatter run. Without it a deferred save would post a fresh run every time it
  // finished one.
  bool skip_formatter_once = false;
  // Set when this tab was closed with unsaved edits and the user chose Save: the
  // save is deferred (the formatter runs on the worker), so the CLOSE has to wait
  // for it too. Closing before the write lands would discard the edits the user
  // just asked to keep. The completion clears it and closes the tab.
  //
  // Per-tab rather than one pending-close on the shell, because two tabs can be
  // closing at once — "Close Others" over a group with several dirty buffers
  // posts one formatter run each, and they complete in whatever order the worker
  // finishes them.
  bool close_after_save = false;
  // The rename or delete of this file is waiting on this tab's deferred save
  // (PromptState::deferred_path_mutation); the formatter completion resumes it
  // once the write lands, exactly as `close_after_save` resumes a close.
  bool path_mutation_after_save = false;
  // Per-tab fold-region model. Lazily computed by the renderer / fold action
  // path through `EnsureFoldingModelFresh(...)`. Cleared automatically on tab
  // close; rekeyed implicitly through its `(layout_revision, tab_size,
  // language_id)` fingerprint when the buffer or language changes.
  std::unique_ptr<editor::FoldingModel> folding_model =
      std::make_unique<editor::FoldingModel>();
  // The caret set the last prepared frame saw, so the frame can tell a caret
  // that MOVED into a collapsed fold (reveal it, as VS Code's revealCursor
  // does) from a fold that collapsed around a caret standing still (leave it).
  std::optional<editor::TextPosition> fold_reveal_last_caret;
  std::size_t fold_reveal_last_secondary_count = 0;
  editor::SnippetSessionState snippet_session;
};

// Namespace scope for the same reason as `EditorTabState` above: it has no NSDMI
// today, but a nested type reachable through an `optional` member of its own
// enclosing class is the landmine, not the initializer.
struct DeferredTabHandle {
  std::filesystem::path path;
  std::size_t cursor_line = 0;
  std::size_t cursor_column = 0;
  std::size_t scroll_line = 0;
  std::size_t horizontal_scroll = 0;
  std::optional<editor::SelectionRange> selection;
};

// Whether a clean reload must re-establish for itself that the file actually
// changed. The guard is not free — settling a moved mtime on an unchanged file
// costs a read — and a caller that has already settled it would otherwise pay for
// the same answer twice. Namespace scope because the shell, the service and the
// coordinator all name it and the shell sees none of the other two as complete
// types.
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

// Whether closing this tab would discard edits: an editable compare's right side,
// a merge's result, or an editor buffer. The one definition the tab coordinator's
// close prompt and the control channel's `editor` query both read.
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

struct TerminalPaneState {
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
  // How this pane was launched, kept so an exited session (a dropped ssh link,
  // a shell that was killed) can be relaunched in place with the same cwd and
  // command rather than opened as a fresh tab that loses its position.
  std::filesystem::path launch_working_directory;
  std::string launch_command;
};

// One tab of the bottom panel's terminal strip: one to `kMaxTerminalPanes`
// sessions laid out side by side (VS Code's terminal group). The strip shows the
// tab; the keyboard, the find bar, the selection and the scroll verbs all act on
// its ACTIVE pane, and every other pane only paints. `panes` and `weights` are
// parallel and edited together through the methods below so a pane can never be
// left without a share of the body (the same discipline `EditorSplitTree` keeps
// with `editor_groups`).
struct TerminalTabState {
  std::vector<std::unique_ptr<TerminalPaneState>> panes;
  TerminalPaneWeights weights;
  std::size_t active_pane = 0;
  // Output arrived while this tab was not the one on screen. Drawn as a dot on
  // the strip tab and cleared the first frame the tab is shown, so an agent that
  // finishes in a background tab is visible without switching to it.
  bool has_unseen_output = false;
  // Bumped by every structural edit and divider move; the per-frame grid resize
  // keys on it instead of re-reading each pane's rows and columns under the
  // session mutex.
  std::uint64_t layout_revision = 0;

  TerminalPaneState* active() {
    return active_pane < panes.size() ? panes[active_pane].get() : nullptr;
  }
  const TerminalPaneState* active() const {
    return active_pane < panes.size() ? panes[active_pane].get() : nullptr;
  }
  std::size_t pane_count() const { return panes.size(); }
  bool full() const { return panes.size() >= kMaxTerminalPanes; }

  // Insert `pane` at `index` (clamped), halving the share of the pane it was
  // split from, and make it the active pane. Returns false (pane untouched) when
  // the tab is full.
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

  // Drop pane `index`, giving its share to its left neighbour. The active pane
  // follows the survivor on the left (or the new first pane). Returns the
  // removed pane so a caller can finish its shutdown; null when `index` is out
  // of range.
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

  // The panes carved out of the panel body, and the active pane's slice of it
  // (the whole body for a single pane).
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

// A tab around one pane: the shape every terminal starts in.
inline std::unique_ptr<TerminalTabState> MakeTerminalTab(std::unique_ptr<TerminalPaneState> pane) {
  auto tab = std::make_unique<TerminalTabState>();
  tab->InsertPane(0, std::move(pane));
  return tab;
}

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
  return editor_state.content_pending() ? editor_state.restored_path
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
