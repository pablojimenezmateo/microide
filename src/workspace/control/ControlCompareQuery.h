#pragma once

// The `compare` control query: what the focused compare tab's model shows, row by
// row -- the line diff's hunks and the structural refinement's verdict on them.
//
// The structural diff (src/compare/HunkRefinement.h) decides which TOKENS of a
// changed line are novel, and a changed line with none is shown as a layout-only
// change. Nothing outside the renderer could read that verdict, so no oracle could
// hold the refinement to what it claims (re-indenting costs nothing; a real change
// is never shown as layout only). This answers from the model the view paints.
//
// Per row: `kind` (unchanged/added/deleted/modified), `hunk` (-1 outside one), the
// 1-based `leftLine`/`rightLine` (0 = absent), both texts, and each side's novel
// byte spans as [start, end) pairs into that side's text. A side that is not
// valid UTF-8 also carries `leftHex`/`rightHex`, its exact bytes: the JSON text
// has U+FFFD for each bad run, so the spans' offsets are only right against
// these. Rows past `max_rows` are dropped (`rowsTruncated: true`). Read-only: no
// state change, no redraw.

#include <cstddef>

#include "util/JsonValue.h"

namespace microide::workspace {

struct CompareTabState;

struct ControlCompareQueryOptions {
  std::size_t max_rows = 20'000;
  // A row's text is cut past this many bytes (`textTruncated: true` on the row);
  // its spans are reported whole.
  std::size_t max_text_bytes = 4096;
};

// Null `compare` answers `{available:false}`.
util::JsonValue BuildControlCompareJson(const CompareTabState* compare,
                                        const ControlCompareQueryOptions& options = {});

}  // namespace microide::workspace
