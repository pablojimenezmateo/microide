#pragma once

#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "editor/EditTypes.h"
#include "editor/TextBuffer.h"
#include "editor/TextLayout.h"

namespace microide::editor {

enum class CoalesceKind { None, Insert, DeleteBackward, DeleteForward };

struct CoalesceHint {
  CoalesceKind kind = CoalesceKind::None;
  bool changed_is_space = false;
};

class TextViewportUndoHistory {
 public:
  struct SecondaryCaret {
    TextPosition position;
    std::size_t preferred_column = 0;
    std::optional<TextPosition> selection_anchor;
    WrapRowAffinity wrap_affinity = WrapRowAffinity::kNextRow;
  };

  struct ViewState {
    std::size_t cursor_line = 0;
    std::size_t cursor_column = 0;
    std::size_t preferred_column = 0;
    std::size_t scroll_line = 0;
    std::size_t horizontal_scroll = 0;
    std::optional<TextPosition> selection_anchor;
    std::vector<SecondaryCaret> secondary_carets;
    bool placeholder = false;
    bool dirty = false;
  };

  struct DisjointPart {
    std::size_t before_start = 0;
    std::size_t after_start = 0;
    LineBlob before_lines;
    LineBlob after_lines;
  };

  struct Entry {
    std::size_t start_line = 0;
    bool is_inline = false;
    std::size_t start_column = 0;
    std::string removed_text;
    std::string inserted_text;
    LineBlob before_lines;
    LineBlob after_lines;
    std::vector<DisjointPart> extra_parts;
    std::optional<util::LineEnding> line_ending_before;
    std::optional<util::LineEnding> line_ending_after;
    ViewState before_state;
    ViewState after_state;
    std::size_t byte_size = 0;

    std::size_t before_line_count() const { return is_inline ? 1 : before_lines.size(); }
    std::size_t after_line_count() const { return is_inline ? 1 : after_lines.size(); }

    bool is_multi_range() const { return !extra_parts.empty(); }

    template <typename Fn>
    void ForEachPartInApplyOrder(bool forward, Fn&& fn) const {
      for (std::size_t i = extra_parts.size(); i-- > 0;) {
        const DisjointPart& part = extra_parts[i];
        fn(forward ? part.before_start : part.after_start,
           forward ? part.before_lines : part.after_lines,
           forward ? part.after_lines : part.before_lines);
      }
      fn(start_line, forward ? before_lines : after_lines, forward ? after_lines : before_lines);
    }
  };

  bool IsGroupActive() const { return !group_stack_.empty(); }
  void BeginGroup(ViewState before_state);
  void RecordEntry(Entry entry, CoalesceHint hint = CoalesceHint{});
  std::optional<Entry> FinishActiveGroup(ViewState after_state);

  const Entry* TopUndoEntry() const { return undo_stack_.empty() ? nullptr : &undo_stack_.back(); }
  bool CanUndo() const { return !undo_stack_.empty(); }
  bool CanRedo() const { return !redo_stack_.empty(); }
  Entry PopUndo();
  Entry PopRedo();
  void PushUndo(Entry entry);
  void PushRedo(Entry entry);
  void ClearRedo() { redo_stack_.clear(); }
  void Clear();
  void MarkSaved();
  void MarkNoPositionSaved();
  bool HasSavedPosition() const;

  std::size_t ApproximateResidentBytes() const;

  void NotifyCursorMoved() { EndCoalesceRun(); }

  static void ApplyEntryToBuffer(TextBuffer& lines, const Entry& entry, bool forward);
  static std::optional<AppliedEdit> BuildAppliedEdit(const Entry& entry, bool forward);
  static std::optional<AppliedEditLineSpan> BuildAppliedEditLineSpan(const Entry& entry,
                                                                     bool forward);
  static Entry BuildEntryForDocumentChange(LineBlob before_lines,
                                            const ViewState& before_state,
                                            LineBlob after_lines,
                                            const ViewState& after_state);

 private:
  struct UndoGroupFrame {
    ViewState state;
    std::vector<Entry> disjoint_entries;
  };

  static bool CanMergeGroupEntry(const Entry& aggregate, const Entry& next);
  static Entry MergeGroupEntry(Entry aggregate, const Entry& next);
  static bool CanMergeInlineEntry(const Entry& aggregate, const Entry& next);
  static bool CanMergeSameShapeMultiRange(const Entry& top, const Entry& next);
  static void MergeSameShapeMultiRange(Entry& top, const Entry& next);
  static void MergeInlineEntryInto(Entry& aggregate, const Entry& next);
  static void MergeChildIntoDisjoint(std::vector<Entry>& entries, Entry next);
  static void MergeChildOverRanges(std::vector<Entry>& entries, std::size_t first,
                                   std::size_t last, Entry next);
  static void InsertSortedDisjoint(std::vector<Entry>& entries, Entry next);
  static void CoalesceAdjacentDisjoint(std::vector<Entry>& entries);
  bool TryCoalesceWithTop(const Entry& next, CoalesceHint hint);
  void EndCoalesceRun() { active_run_kind_ = CoalesceKind::None; }
  void AppendUndoEntry(Entry entry);
  void EnforceHistoryBudget();

  static constexpr std::size_t kMaxHistoryEntries = 128;
  static constexpr std::size_t kMaxHistoryBytes = 256u << 20;

  std::deque<Entry> undo_stack_;
  std::deque<Entry> redo_stack_;
  std::vector<UndoGroupFrame> group_stack_;
  CoalesceKind active_run_kind_ = CoalesceKind::None;
  bool active_run_last_space_ = false;
};

}
