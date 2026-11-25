#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "editor/LineSpan.h"
#include "editor/TextViewport.h"

namespace microide::editor::detail {

inline constexpr std::size_t kHighlightCheckpointInterval = 128;

inline constexpr struct PositionLessFn {
  bool operator()(const TextPosition& lhs, const TextPosition& rhs) const {
    if (lhs.line != rhs.line) {
      return lhs.line < rhs.line;
    }
    return lhs.column < rhs.column;
  }
} PositionLess{};

inline constexpr struct SecondaryCaretPositionLessFn {
  bool operator()(const TextViewportUndoHistory::SecondaryCaret& lhs,
                  const TextViewportUndoHistory::SecondaryCaret& rhs) const {
    return PositionLess(lhs.position, rhs.position);
  }
} SecondaryCaretPositionLess{};

inline void SortSecondaryCaretsIfNeeded(
    std::vector<TextViewportUndoHistory::SecondaryCaret>& carets) {
  if (!std::is_sorted(carets.begin(), carets.end(), SecondaryCaretPositionLess)) {
    std::sort(carets.begin(), carets.end(), SecondaryCaretPositionLess);
  }
}

inline std::optional<SelectionRange> SelectionRangeForSecondaryCaret(
    const TextPosition& position,
    const std::optional<TextPosition>& selection_anchor) {
  if (!selection_anchor.has_value()) {
    return std::nullopt;
  }
  if (selection_anchor->line == position.line && selection_anchor->column == position.column) {
    return std::nullopt;
  }
  if (PositionLess(*selection_anchor, position)) {
    return SelectionRange{*selection_anchor, position};
  }
  return SelectionRange{position, *selection_anchor};
}

inline TextPosition RangeEndExclusive(const SelectionRange& r) {
  return PositionLess(r.start, r.end) ? r.end : r.start;
}

inline bool ValidateRangeColumns(LineSpan lines, const SelectionRange& n) {
  if (n.start.line >= lines.size() || n.end.line >= lines.size()) {
    return false;
  }
  if (n.start.column > lines[n.start.line].size() ||
      n.end.column > lines[n.end.line].size()) {
    return false;
  }
  return true;
}

inline std::string TextBetweenLines(LineSpan lines, const SelectionRange& n) {
  const auto& a = n.start;
  const auto& b = n.end;
  if (a.line == b.line) {
    return std::string(lines[a.line].substr(a.column, b.column - a.column));
  }
  std::string out;
  out += lines[a.line].substr(a.column);
  out.push_back('\n');
  for (std::size_t i = a.line + 1; i < b.line; ++i) {
    out += lines[i];
    out.push_back('\n');
  }
  out += lines[b.line].substr(0, b.column);
  return out;
}

struct ReplacementShape {
  std::size_t inserted_newlines = 0;
  std::size_t last_segment_cols = 0;
};

inline ReplacementShape ComputeReplacementShape(std::string_view replacement) {
  ReplacementShape shape;
  for (std::size_t i = 0; i < replacement.size(); ++i) {
    const char ch = replacement[i];
    if (ch == '\n') {
      ++shape.inserted_newlines;
      shape.last_segment_cols = 0;
    } else if (ch == '\r') {
      ++shape.inserted_newlines;
      shape.last_segment_cols = 0;
      if (i + 1 < replacement.size() && replacement[i + 1] == '\n') {
        ++i;
      }
    } else {
      ++shape.last_segment_cols;
    }
  }
  return shape;
}

inline TextPosition RemapPositionAfterReplace(TextPosition position,
                                              TextPosition removed_start,
                                              TextPosition removed_end,
                                              const ReplacementShape& shape) {
  if (PositionLess(position, removed_end)) {
    return position;
  }

  if (position.line == removed_end.line) {
    TextPosition result;
    result.line = removed_start.line + shape.inserted_newlines;
    const std::size_t tail = position.column - removed_end.column;
    result.column = shape.inserted_newlines == 0
                        ? removed_start.column + shape.last_segment_cols + tail
                        : shape.last_segment_cols + tail;
    return result;
  }

  const std::ptrdiff_t line_delta =
      static_cast<std::ptrdiff_t>(shape.inserted_newlines) -
      static_cast<std::ptrdiff_t>(removed_end.line - removed_start.line);
  TextPosition result = position;
  result.line = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(position.line) + line_delta);
  return result;
}

struct MultiCaretRemapSite {
  TextPosition landed{};
  std::optional<TextPosition> anchor;
  bool has_edit = false;
  SelectionRange removed{};
  ReplacementShape shape{};
};

inline void ResolveMultiCaretRemapSites(std::vector<MultiCaretRemapSite>& sites) {
  bool fast_eligible = true;
  for (const MultiCaretRemapSite& site : sites) {
    if (site.anchor.has_value() ||
        (site.has_edit && site.removed.start.line != site.removed.end.line)) {
      fast_eligible = false;
      break;
    }
  }

  if (!fast_eligible) {
    for (std::size_t i = 0; i < sites.size(); ++i) {
      for (std::size_t k = i; k-- > 0;) {
        if (!sites[k].has_edit) {
          continue;
        }
        const SelectionRange& removed = sites[k].removed;
        sites[i].landed =
            RemapPositionAfterReplace(sites[i].landed, removed.start, removed.end, sites[k].shape);
        if (sites[i].anchor.has_value()) {
          sites[i].anchor = RemapPositionAfterReplace(*sites[i].anchor, removed.start, removed.end,
                                                      sites[k].shape);
        }
      }
    }
    return;
  }

  std::ptrdiff_t acc_line = 0;
  std::ptrdiff_t acc_col = 0;
  std::size_t prev_orig_line = std::numeric_limits<std::size_t>::max();
  for (MultiCaretRemapSite& site : sites) {
    const std::size_t orig_line =
        site.has_edit ? site.removed.start.line : site.landed.line;
    if (orig_line != prev_orig_line) {
      acc_col = 0;
      prev_orig_line = orig_line;
    }
    const std::size_t final_line =
        static_cast<std::size_t>(static_cast<std::ptrdiff_t>(site.landed.line) + acc_line);
    std::size_t final_col = site.landed.column;
    if (site.landed.line == orig_line) {
      final_col = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(final_col) + acc_col);
    }
    site.landed = TextPosition{final_line, final_col};

    if (site.has_edit) {
      acc_line += static_cast<std::ptrdiff_t>(site.shape.inserted_newlines);
      if (site.shape.inserted_newlines == 0) {
        acc_col += static_cast<std::ptrdiff_t>(site.shape.last_segment_cols) -
                   static_cast<std::ptrdiff_t>(site.removed.end.column - site.removed.start.column);
      } else {
        acc_col = static_cast<std::ptrdiff_t>(site.shape.last_segment_cols) -
                  static_cast<std::ptrdiff_t>(site.removed.end.column);
      }
    }
  }
}

class DisjointEditCapture {
 public:
  explicit DisjointEditCapture(const TextBuffer& buffer) : buffer_(&buffer) {}

  struct LineRange {
    std::size_t first = 0;
    std::size_t last = 0;
  };

  void PlanFootprints(std::span<const std::optional<LineRange>> site_spans) {
    footprints_.clear();
    opens_at_.assign(site_spans.size(), kNoFootprint);
    closes_at_.assign(site_spans.size(), kNoFootprint);
    for (std::size_t site = 0; site < site_spans.size(); ++site) {
      if (!site_spans[site].has_value()) {
        continue;
      }
      const std::size_t start = site_spans[site]->first;
      const std::size_t end_exclusive = site_spans[site]->last + 1;
      if (!footprints_.empty() && start <= footprints_.back().end_exclusive) {
        footprints_.back().end_exclusive = std::max(footprints_.back().end_exclusive, end_exclusive);
        footprints_.back().last_site = site;
        continue;
      }
      footprints_.push_back(Footprint{.start = start,
                                      .end_exclusive = end_exclusive,
                                      .first_site = site,
                                      .last_site = site});
    }
    for (std::size_t f = 0; f < footprints_.size(); ++f) {
      opens_at_[footprints_[f].last_site] = f;
      closes_at_[footprints_[f].first_site] = f;
    }
  }

  void EnterSite(std::size_t site) {
    if (site < opens_at_.size() && opens_at_[site] != kNoFootprint) {
      const Footprint& footprint = footprints_[opens_at_[site]];
      BeginFootprint(footprint.start, footprint.end_exclusive);
    }
  }

  void LeaveSite(std::size_t site) {
    if (site < closes_at_.size() && closes_at_[site] != kNoFootprint) {
      EndFootprint();
    }
  }

  bool empty() const { return parts_.empty(); }

  TextViewportUndoHistory::Entry Build(
      const TextViewportUndoHistory::ViewState& before_state,
      const TextViewportUndoHistory::ViewState& after_state) {
    std::reverse(parts_.begin(), parts_.end());
    TextViewportUndoHistory::Entry aggregate;
    aggregate.before_state = before_state;
    aggregate.after_state = after_state;
    std::ptrdiff_t shift = 0;
    bool have_primary = false;
    for (Captured& part : parts_) {
      TextViewportUndoHistory::Entry trimmed =
          TextViewportUndoHistory::BuildEntryForDocumentChange(
              std::move(part.before_lines), before_state, std::move(part.after_lines), after_state);
      if (trimmed.before_lines.empty() && trimmed.after_lines.empty()) {
        continue;
      }
      const std::size_t before_start = part.before_start + trimmed.start_line;
      const std::ptrdiff_t part_delta = static_cast<std::ptrdiff_t>(trimmed.after_lines.size()) -
                                        static_cast<std::ptrdiff_t>(trimmed.before_lines.size());
      if (!have_primary) {
        aggregate.start_line = before_start;
        aggregate.before_lines = std::move(trimmed.before_lines);
        aggregate.after_lines = std::move(trimmed.after_lines);
        have_primary = true;
      } else {
        aggregate.extra_parts.push_back(TextViewportUndoHistory::DisjointPart{
            .before_start = before_start,
            .after_start = static_cast<std::size_t>(
                static_cast<std::ptrdiff_t>(before_start) + shift),
            .before_lines = std::move(trimmed.before_lines),
            .after_lines = std::move(trimmed.after_lines),
        });
      }
      shift += part_delta;
    }
    return aggregate;
  }

 private:
  void BeginFootprint(std::size_t start, std::size_t end_exclusive) {
    open_start_ = std::min(start, buffer_->size());
    open_before_size_ = std::clamp(end_exclusive, open_start_, buffer_->size()) - open_start_;
    document_size_at_begin_ = buffer_->size();
    open_before_lines_ = buffer_->SliceLinesBlob(open_start_, open_start_ + open_before_size_);
    open_ = true;
  }

  void EndFootprint() {
    if (!open_) {
      return;
    }
    open_ = false;
    const std::ptrdiff_t delta = static_cast<std::ptrdiff_t>(buffer_->size()) -
                                 static_cast<std::ptrdiff_t>(document_size_at_begin_);
    const std::size_t after_size = static_cast<std::size_t>(std::max<std::ptrdiff_t>(
        0, static_cast<std::ptrdiff_t>(open_before_size_) + delta));
    const std::size_t after_end = std::min(buffer_->size(), open_start_ + after_size);
    parts_.push_back(Captured{
        .before_start = open_start_,
        .before_lines = std::move(open_before_lines_),
        .after_lines = buffer_->SliceLinesBlob(std::min(open_start_, after_end), after_end),
    });
  }

  static constexpr std::size_t kNoFootprint = std::numeric_limits<std::size_t>::max();
  struct Footprint {
    std::size_t start = 0;
    std::size_t end_exclusive = 0;
    std::size_t first_site = 0;
    std::size_t last_site = 0;
  };
  std::vector<Footprint> footprints_;
  std::vector<std::size_t> opens_at_;
  std::vector<std::size_t> closes_at_;
  struct Captured {
    std::size_t before_start = 0;
    LineBlob before_lines;
    LineBlob after_lines;
  };

  const TextBuffer* buffer_;
  std::vector<Captured> parts_;
  LineBlob open_before_lines_;
  std::size_t open_start_ = 0;
  std::size_t open_before_size_ = 0;
  std::size_t document_size_at_begin_ = 0;
  bool open_ = false;
};

}
