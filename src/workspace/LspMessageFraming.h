#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "util/JsonValue.h"

// LSP wire framing: the `Content-Length: N\r\n\r\n<body>` codec that turns a raw
// stdout byte stream into discrete JSON-RPC messages. Extracted from the transport
// `Impl` so the parser — a hot path (speed) and a hostile-input surface
// (correctness: oversized/malformed frames must resync, never desync) — is a pure
// value type with deterministic unit coverage instead of only reachable through a
// live subprocess.
namespace microide::workspace {

// ---------------------------------------------------------------------------
// Internal buffer — avoids O(n) prefix-erasure on every line read.
// ---------------------------------------------------------------------------
struct ReadBuf {
  std::string data;
  std::size_t pos = 0;

  std::string_view view() const { return std::string_view(data).substr(pos); }

  void consume(std::size_t n) {
    pos += n;
    // Compact when the consumed prefix is large to bound memory usage.
    if (pos > 65536 && pos > data.size() / 2) {
      data.erase(0, pos);
      pos = 0;
    }
  }

  void append(std::string_view chunk) { data.append(chunk); }
};

// Incrementally frames a Content-Length-delimited JSON-RPC stream. Feed bytes with
// Append(); pull complete messages with Next() until it returns nullopt (need more
// bytes). A single framer instance carries the cross-chunk state (partial frame +
// oversized-frame skip counter), so it is used as a member and survives the
// initialize-handshake -> I/O-thread handoff without dropping server-pushed bytes.
struct LspMessageFramer {
  ReadBuf buf;
  // Remaining body bytes to drain-and-discard for a frame whose declared
  // Content-Length exceeded kMaxLspMessageBytes. Skipping the whole frame lets the
  // parser resync to the next frame instead of reading body bytes as headers (which
  // would desync the stream and tear the session down).
  std::size_t skip_body_bytes = 0;

  void Append(std::string_view chunk) { buf.append(chunk); }

  // Bytes buffered but not yet framed — the caller compares this against the
  // read-buffer cap to tear down a runaway server that never frames a message.
  std::size_t BufferedBytes() const { return buf.view().size(); }

  // Parse one complete message from the buffer, consuming it. Returns nullopt when
  // more bytes are needed, when a header line is skipped for resync, or while an
  // oversized frame is being drained.
  std::optional<util::JsonValue> Next();
};

}  // namespace microide::workspace
