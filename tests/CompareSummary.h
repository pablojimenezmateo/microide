#pragma once

#include <cstddef>

#include "compare/CompareModel.h"

namespace microide::tests {

struct CompareSummary {
  int unchanged = 0;
  int added = 0;
  int deleted = 0;
  int modified = 0;
};

inline CompareSummary Summarize(const microide::compare::CompareModel& model) {
  CompareSummary summary;
  for (const auto& row : model.rows) {
    switch (row.kind) {
      case microide::compare::CompareRowKind::Unchanged:
        ++summary.unchanged;
        break;
      case microide::compare::CompareRowKind::Added:
        ++summary.added;
        break;
      case microide::compare::CompareRowKind::Deleted:
        ++summary.deleted;
        break;
      case microide::compare::CompareRowKind::Modified:
        ++summary.modified;
        break;
    }
  }
  return summary;
}

}
