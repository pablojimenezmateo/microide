#pragma once

// The `buffer` control query: what the focused editor holds, for a driver that
// has to check an edit landed rather than that a frame painted.
//
// tools/qa's flows (typing, multi-cursor edits, find/replace, folding, undo) used
// to verify by saving and reading the file back, which cannot see a selection, a
// secondary caret or a fold, and which an edit the save path normalized hides.
// This answers from the live viewport: its text, every caret with its selection
// (primary first), the collapsed folds and the foldable openers in the resolved
// window, and the buffer's own flags.
//
// Positions are 1-based like the rest of the control surface; columns are byte
// columns, as `editor`'s cursorColumn. Read-only: it changes no state and requests
// no redraw.

#include <cstddef>

#include "util/JsonValue.h"

namespace microide::editor {
class TextViewport;
}

namespace microide::workspace {

struct ControlBufferQueryOptions {
  // The text is omitted (with `textTruncated: true`) past this many bytes, so a
  // query against a huge file stays a bounded answer on the UI thread.
  std::size_t max_text_bytes = 1u << 20;
  // Caret entries past this many are dropped (`cursorsTruncated: true`).
  std::size_t max_cursors = 1000;
};

// The viewport the caret is in (the focused group's active tab: an editor, a
// compare tab's editable side, a merge result) and the tab kind it belongs to.
struct ControlActiveBuffer {
  const editor::TextViewport* viewport = nullptr;
  const char* kind = "editor";
};

// `kind` names the surface the viewport belongs to ("editor", "compare",
// "merge"); null `viewport` answers `{available:false}`.
util::JsonValue BuildControlBufferJson(const editor::TextViewport* viewport,
                                       const char* kind,
                                       const ControlBufferQueryOptions& options = {});

}  // namespace microide::workspace
