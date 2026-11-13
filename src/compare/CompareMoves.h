#pragma once

#include "compare/CompareModel.h"

namespace microide::compare {

// Marks moved lines (TD-2026-11-11-443): a run of whole deleted rows in one hunk
// whose text, indentation aside, reappears as a run of whole added rows in
// another hunk -- git's --color-moved, with its 20-alphanumeric minimum per
// block. Sets CompareRow::left_moved / right_moved; clears them everywhere else.
// Linear in the rows (a hash per added line, a bounded candidate list).
void MarkMovedLines(CompareModel& model);

}  // namespace microide::compare
