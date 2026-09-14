#pragma once

// Row-kind tally for a CompareModel, shared by the tests that assert what a diff
// produced.
//
// CompareModelTests and GitServiceTests each grew a byte-identical private copy
// of this -- the drift shape tools/clone-scan.py exists to find: two files
// counting the same four row kinds, and nothing stopping one of them from being
// taught about a fifth.

#include <cstddef>

#include "compare/CompareModel.h"

namespace microide::tests {

struct CompareSummary {
  int unchanged = 0;
  int added = 0;
  int deleted = 0;
  int modified = 0;
};

// No `default:` on purpose: a new CompareRowKind must fail to compile here
// rather than be silently counted as nothing by both callers.
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

}  // namespace microide::tests
