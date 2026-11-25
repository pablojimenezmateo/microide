#pragma once

#include <cstddef>

#include "util/JsonValue.h"

namespace microide::editor {
class TextViewport;
}

namespace microide::workspace {

struct ControlBufferQueryOptions {
  std::size_t max_text_bytes = 1u << 20;
  std::size_t max_cursors = 1000;
};

struct ControlActiveBuffer {
  const editor::TextViewport* viewport = nullptr;
  const char* kind = "editor";
};

util::JsonValue BuildControlBufferJson(const editor::TextViewport* viewport,
                                       const char* kind,
                                       const ControlBufferQueryOptions& options = {});

}
