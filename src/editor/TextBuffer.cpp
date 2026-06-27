#include "editor/TextBuffer.h"

// TextBuffer is now a header-only forwarder over PieceTree (see TextBuffer.h);
// all line storage and mutation live in PieceTree. This translation unit is kept
// so the build's source list and any explicit-instantiation hooks stay stable.

namespace microide::editor {}  // namespace microide::editor
