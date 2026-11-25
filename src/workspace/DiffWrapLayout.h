#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "editor/TextLayout.h"

namespace microide::workspace {

struct DiffWrapRow {
  std::uint32_t unit = 0;
  std::uint32_t left_start = 0;
  std::uint32_t left_end = 0;
  std::uint32_t left_indent = 0;
  std::uint32_t right_start = 0;
  std::uint32_t right_end = 0;
  std::uint32_t right_indent = 0;
  bool first = false;
  bool left_present = false;
  bool right_present = false;
};

class DiffWrapLayout {
 public:
  struct UnitText {
    std::string_view left;
    std::string_view right;
    bool has_left = false;
    bool has_right = false;
    bool single_row = false;
  };

  bool active() const { return active_; }
  std::size_t built_left_columns() const { return built_left_columns_; }
  std::size_t built_right_columns() const { return built_right_columns_; }

  template <typename TextForUnit>
  void Ensure(std::uint64_t content_revision,
              bool soft_wrap,
              std::size_t left_columns,
              std::size_t right_columns,
              std::size_t tab_size,
              std::size_t unit_count,
              TextForUnit&& text_for_unit) const {
    const std::size_t left_wrap = left_columns == 0 ? 1 : left_columns;
    const std::size_t right_wrap = right_columns == 0 ? 1 : right_columns;
    if (!soft_wrap) {
      if (active_) {
        Reset();
      }
      return;
    }
    if (active_ && built_revision_ == content_revision && built_left_columns_ == left_wrap &&
        built_right_columns_ == right_wrap && built_tab_size_ == tab_size &&
        built_unit_count_ == unit_count) {
      return;
    }

    rows_.clear();
    first_row_.clear();
    rows_.reserve(unit_count + unit_count / 4);
    first_row_.reserve(unit_count + 1);

    for (std::size_t unit = 0; unit < unit_count; ++unit) {
      first_row_.push_back(static_cast<std::uint32_t>(rows_.size()));
      const UnitText text = text_for_unit(unit);
      if (text.single_row) {
        DiffWrapRow row;
        row.unit = static_cast<std::uint32_t>(unit);
        row.first = true;
        row.left_present = text.has_left;
        row.right_present = text.has_right;
        rows_.push_back(row);
        continue;
      }

      const std::size_t first_row_index = rows_.size();
      std::size_t left_rows = 0;
      if (text.has_left) {
        editor::TextLayout::WrapLineSegments(
            text.left, tab_size, left_wrap,
            [&](std::size_t start, std::size_t end, std::size_t indent) {
              DiffWrapRow row;
              row.unit = static_cast<std::uint32_t>(unit);
              row.first = left_rows == 0;
              row.left_present = true;
              row.left_start = static_cast<std::uint32_t>(start);
              row.left_end = static_cast<std::uint32_t>(end);
              row.left_indent = static_cast<std::uint32_t>(indent);
              rows_.push_back(row);
              ++left_rows;
            });
      }

      std::size_t right_rows = 0;
      if (text.has_right) {
        editor::TextLayout::WrapLineSegments(
            text.right, tab_size, right_wrap,
            [&](std::size_t start, std::size_t end, std::size_t indent) {
              const std::size_t index = first_row_index + right_rows;
              if (index == rows_.size()) {
                DiffWrapRow row;
                row.unit = static_cast<std::uint32_t>(unit);
                row.first = right_rows == 0;
                rows_.push_back(row);
              }
              DiffWrapRow& row = rows_[index];
              row.right_present = true;
              row.right_start = static_cast<std::uint32_t>(start);
              row.right_end = static_cast<std::uint32_t>(end);
              row.right_indent = static_cast<std::uint32_t>(indent);
              ++right_rows;
            });
      }

      if (left_rows == 0 && right_rows == 0) {
        DiffWrapRow row;
        row.unit = static_cast<std::uint32_t>(unit);
        row.first = true;
        rows_.push_back(row);
      }
    }
    first_row_.push_back(static_cast<std::uint32_t>(rows_.size()));

    active_ = true;
    built_revision_ = content_revision;
    built_left_columns_ = left_wrap;
    built_right_columns_ = right_wrap;
    built_tab_size_ = tab_size;
    built_unit_count_ = unit_count;
  }

  void Reset() const {
    active_ = false;
    rows_.clear();
    first_row_.clear();
    built_revision_ = 0;
    built_left_columns_ = 0;
    built_right_columns_ = 0;
    built_tab_size_ = 0;
    built_unit_count_ = 0;
  }

  std::size_t RowCount(std::size_t unit_count) const {
    return active_ ? rows_.size() : unit_count;
  }

  DiffWrapRow RowAt(std::size_t visual_row) const {
    if (!active_) {
      DiffWrapRow row;
      row.unit = static_cast<std::uint32_t>(visual_row);
      row.first = true;
      row.left_present = true;
      row.right_present = true;
      return row;
    }
    if (visual_row >= rows_.size()) {
      return DiffWrapRow{};
    }
    return rows_[visual_row];
  }

  std::size_t UnitForRow(std::size_t visual_row) const {
    if (!active_) {
      return visual_row;
    }
    if (rows_.empty()) {
      return 0;
    }
    return rows_[std::min(visual_row, rows_.size() - 1)].unit;
  }

  std::size_t FirstRowForUnit(std::size_t unit) const {
    if (!active_) {
      return unit;
    }
    if (first_row_.size() < 2) {
      return 0;
    }
    return first_row_[std::min(unit, first_row_.size() - 2)];
  }

  std::size_t RowForUnitColumn(std::size_t unit, std::size_t visual_column, bool right_side) const {
    if (!active_) {
      return unit;
    }
    const std::size_t first = FirstRowForUnit(unit);
    const std::size_t span = RowSpanForUnit(unit);
    std::size_t fallback = first;
    for (std::size_t i = 0; i < span && first + i < rows_.size(); ++i) {
      const DiffWrapRow& row = rows_[first + i];
      const bool present = right_side ? row.right_present : row.left_present;
      if (!present) {
        continue;
      }
      fallback = first + i;
      const std::size_t start = right_side ? row.right_start : row.left_start;
      const std::size_t end = right_side ? row.right_end : row.left_end;
      if (visual_column >= start && visual_column < end) {
        return first + i;
      }
    }
    return fallback;
  }

  std::size_t RowSpanForUnit(std::size_t unit) const {
    if (!active_) {
      return 1;
    }
    if (first_row_.size() < 2 || unit + 1 >= first_row_.size()) {
      return 1;
    }
    return first_row_[unit + 1] - first_row_[unit];
  }

 private:
  mutable std::vector<DiffWrapRow> rows_;
  mutable std::vector<std::uint32_t> first_row_;
  mutable std::uint64_t built_revision_ = 0;
  mutable std::size_t built_left_columns_ = 0;
  mutable std::size_t built_right_columns_ = 0;
  mutable std::size_t built_tab_size_ = 0;
  mutable std::size_t built_unit_count_ = 0;
  mutable bool active_ = false;
};

}
