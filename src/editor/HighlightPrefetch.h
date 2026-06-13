#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "editor/SyntaxHighlighter.h"

namespace microide::editor {

class TextViewport;

// An immutable snapshot of the lines that need highlighting plus the state to
// resume from — safe to hand to a worker thread by value. The `viewport`
// pointer is only an identity token compared on the main thread when installing
// results; the worker never dereferences it (the lines are a deep copy).
struct HighlightPrefetchRequest {
  const TextViewport* viewport = nullptr;
  std::filesystem::path path;
  std::uint64_t content_revision = 0;
  std::uint64_t syntax_revision = 0;
  SyntaxState start_state;          // resume state for the line before start_line
  std::size_t start_line = 0;
  std::vector<std::string> lines;   // deep copy of [start_line, start_line + count)
};

// Result of tokenizing a HighlightPrefetchRequest off-thread. Installed back
// into the originating viewport's cache on the main thread iff the revisions
// still match (see TextViewport::InstallPrefetchedHighlights).
struct HighlightPrefetchResult {
  const TextViewport* viewport = nullptr;
  std::uint64_t content_revision = 0;
  std::uint64_t syntax_revision = 0;
  std::size_t start_line = 0;
  std::vector<std::vector<SyntaxTokenKind>> tokens;  // one entry per snapshot line
  std::vector<SyntaxState> end_states;               // one entry per snapshot line
};

// Pure, thread-safe tokenization of a request (uses only the immutable syntax
// registry and the request's own snapshot). Intended to run on a worker thread.
HighlightPrefetchResult ComputeHighlightPrefetch(const HighlightPrefetchRequest& request);

}  // namespace microide::editor
