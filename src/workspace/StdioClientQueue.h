#pragma once

#include <functional>
#include <string>
#include <utility>

namespace microide::workspace {

// A message queued for the stdio I/O thread of an LSP or DAP client. Either the
// bytes are already serialized, or a builder produces them lazily on the I/O
// thread (so serialization cost is paid off the calling thread). Shared by both
// clients, which drive byte-identical outbound queues.
struct StdioQueuedMessage {
  std::string serialized;
  std::function<std::string()> build_serialized;

  std::string TakeSerialized() && {
    if (!serialized.empty()) {
      return std::move(serialized);
    }
    return build_serialized ? build_serialized() : std::string{};
  }
};

}  // namespace microide::workspace
