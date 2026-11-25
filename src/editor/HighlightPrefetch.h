#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "editor/SyntaxHighlighter.h"

namespace microide::editor {

class TextViewport;

struct HighlightPrefetchRequest {
  const TextViewport* viewport = nullptr;
  std::filesystem::path path;
  std::uint64_t content_revision = 0;
  std::uint64_t syntax_revision = 0;
  SyntaxState start_state;
  std::size_t start_line = 0;
  std::vector<std::string> lines;
};

struct HighlightPrefetchResult {
  const TextViewport* viewport = nullptr;
  std::uint64_t content_revision = 0;
  std::uint64_t syntax_revision = 0;
  std::size_t start_line = 0;
  std::vector<std::vector<SyntaxTokenKind>> tokens;
  std::vector<SyntaxState> end_states;
};

HighlightPrefetchResult ComputeHighlightPrefetch(const HighlightPrefetchRequest& request);

struct HighlightCheckpointRequest {
  const TextViewport* viewport = nullptr;
  std::filesystem::path path;
  std::uint64_t content_revision = 0;
  std::uint64_t syntax_revision = 0;
  SyntaxState start_state;
  std::size_t first_line = 0;
  std::size_t checkpoint_interval = 0;
  std::vector<std::string> lines;
};

struct HighlightCheckpointResult {
  const TextViewport* viewport = nullptr;
  std::uint64_t content_revision = 0;
  std::uint64_t syntax_revision = 0;
  std::size_t first_checkpoint_index = 0;
  std::vector<SyntaxState> checkpoint_states;
};

HighlightCheckpointResult ComputeHighlightCheckpoints(const HighlightCheckpointRequest& request);

}
