#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace microide::compare {

enum class CompareRowKind {
  Unchanged,
  Added,
  Deleted,
  Modified,
};

struct CompareTextSpan {
  std::size_t start = 0;
  std::size_t end = 0;
};

struct CompareRow {
  std::string left_text;
  std::string right_text;
  int left_line = 0;
  int right_line = 0;
  CompareRowKind kind = CompareRowKind::Unchanged;
  int hunk = -1;
  std::vector<CompareTextSpan> left_changed_spans;
  std::vector<CompareTextSpan> right_changed_spans;
};

struct CompareHunk {
  int index = 0;
  int start_row = 0;
  int end_row = 0;
};

struct CompareModel {
  std::vector<CompareRow> rows;
  std::vector<CompareHunk> hunks;
};

CompareModel BuildCompareModel(const std::string& left, const std::string& right);

}  // namespace microide::compare
