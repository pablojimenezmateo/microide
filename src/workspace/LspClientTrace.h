#pragma once

#include <chrono>
#include <cstddef>
#include <string_view>

// Shared tuning constants + lifecycle tracing for the LSP client transport.
// These used to live in an anonymous namespace inside WorkspaceLspClientInternal.h,
// which gave every includer its own internal-linkage copy — two steady_clock trace
// epochs, duplicated constants. Hoisting them here makes the constants single-source
// `inline constexpr` and the trace timeline share one epoch across all TUs.
namespace microide::workspace {

// A request that gets no response within this deadline is failed synthetically so
// the UI's loading state clears instead of hanging forever on a silent server.
// LSP requests here are all post-initialize interactive queries (hover, completion,
// definition, formatting, …); a single generous deadline fits them all. The
// blocking initialize handshake runs on its own thread and is not swept here.
inline constexpr std::chrono::milliseconds kLspRequestTimeout{30000};

// OOM backstop for the outbound/deferred queues. In normal operation the I/O thread
// drains outbound continuously, so this is never approached. The queues only grow
// without bound if the server stops consuming its stdin while staying alive — a
// wedged server. At that point the session is effectively dead (document sync is
// already broken because the server isn't reading), so we refuse further messages
// rather than silently DROP queued protocol messages (which would corrupt sync on a
// healthy stream) or grow until the whole IDE OOMs. Refused requests fail cleanly via
// the send-failure path; the timeout sweep clears anything already pending.
inline constexpr std::size_t kMaxQueuedMessages = 50000;

// Language servers are external, possibly-buggy or hostile processes. Bound a
// single decoded message body and, with slack, the whole read-accumulation
// buffer. Without this, a server can declare a near-INT_MAX Content-Length, or
// stream bytes that never frame a message (no newline / an undelivered body), and
// the accumulation buffer grows without limit -> OOM. A body over the message cap
// is rejected; a read buffer past the buffer cap tears the (already-broken)
// session down. 64 MiB dwarfs any real LSP message.
inline constexpr std::size_t kMaxLspMessageBytes = 64ull * 1024 * 1024;
inline constexpr std::size_t kMaxLspReadBufferBytes = kMaxLspMessageBytes + (1ull * 1024 * 1024);

// Emit a lifecycle trace line to stderr when MICROIDE_TRACE_LSP_LIFECYCLE is set to
// a non-empty, non-"0" value. Lines read as a timeline ("[lsp] +0ms ... / +2400ms
// ...") sharing one process-wide epoch, so the reader sees exactly where the seconds
// between spawn and Ready go. A no-op when tracing is disabled.
void TraceLspLifecycle(std::string_view language_id, int pid, std::string_view phase,
                       std::string_view detail = {});

}  // namespace microide::workspace
