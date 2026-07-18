#pragma once

#include <cstddef>
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
  // Approximate payload bytes this message will serialize to, charged against the
  // client's aggregate outbound byte budget at enqueue (document-sync messages
  // capture their text by value, so the count cap alone can't bound retained bytes
  // when a wedged server stops reading). 0 for small/control frames.
  // TD-2026-07-17A-071.
  std::size_t approx_bytes = 0;

  std::string TakeSerialized() && {
    if (!serialized.empty()) {
      return std::move(serialized);
    }
    return build_serialized ? build_serialized() : std::string{};
  }
};

}  // namespace microide::workspace
