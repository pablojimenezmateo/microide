#pragma once

#include <string>

#include "editor/RuntimeSyntaxRegistry.h"
#include "editor/TextViewport.h"

namespace microide::workspace {

// Single definition of "what language is this buffer" used by every LSP/assist
// path. Extension/content detection runs through the shared runtime syntax
// registry; callers that hit this per-frame should cache the result by path.
inline std::string DetectViewportLanguageId(const editor::TextViewport& viewport) {
  // TD-2026-07-17-072: pass the TextBuffer as a LineSpan (zero-copy, reads through
  // LineView) instead of Snapshot(). Signature detection inspects only a bounded
  // head, so this never materializes the whole document — the previous Snapshot()
  // paid an O(document) copy on every completion/hover/signature/outline path.
  return editor::runtime_syntax::DetectFiletype(viewport.path(),
                                                editor::LineSpan(viewport.lines()));
}

}  // namespace microide::workspace
