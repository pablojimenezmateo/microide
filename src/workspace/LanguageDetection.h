#pragma once

#include <string>

#include "editor/RuntimeSyntaxRegistry.h"
#include "editor/TextViewport.h"

namespace microide::workspace {

// Single definition of "what language is this buffer" used by every LSP/assist
// path. Extension/content detection runs through the shared runtime syntax
// registry; callers that hit this per-frame should cache the result by path.
inline std::string DetectViewportLanguageId(const editor::TextViewport& viewport) {
  return editor::runtime_syntax::DetectFiletype(viewport.path(), viewport.lines());
}

}  // namespace microide::workspace
