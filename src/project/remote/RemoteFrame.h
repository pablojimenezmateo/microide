#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace microide::project::remote {

// The remote server's wire unit (dev-docs/design/remote-projects.md § 6.4): a fixed
// little-endian header — u32 payload length, u16 type, u8 lane, u64 id — and then
// exactly `length` payload bytes. Control frames carry a JSON body; content frames
// carry raw bytes with no encoding step, because a process's stdout split at a read
// boundary routinely cuts a UTF-8 sequence in half, and no JSON string can carry
// half a character.
inline constexpr std::size_t kFrameHeaderBytes = 4 + 2 + 1 + 8;
// The per-frame ceiling. Larger content is chunked by its sender; a header that
// announces more ends the connection (it is either a bug or an attack).
inline constexpr std::uint32_t kMaxFramePayloadBytes = 64u * 1024u * 1024u;

enum class FrameType : std::uint16_t {
  // Control: a JSON body.
  Request = 1,       // {"method": ..., "params": ...}; id = the request id
  Response = 2,      // {"result": ...} or {"error": {"code", "message"}}; id = request id
  Notification = 3,  // {"method": ..., "params": ...}; id = 0, or the handle it concerns
  // Transport-internal, consumed by RemoteFrameTransport and never delivered:
  // binary payloads, always on the interactive lane.
  Ack = 4,   // u64 LE: total bulk payload bytes received so far (the bulk window)
  // Content: raw bytes; id = the process handle.
  ProcStdin = 16,
  ProcStdout = 17,
  ProcStderr = 18,
};

// Whether `type` is one this build knows. An unknown type is a protocol error
// rather than a frame to skip: the version handshake is what admits a peer, so a
// type outside it means the peers do not agree on the wire.
bool IsKnownFrameType(std::uint16_t type);
bool IsContentFrameType(FrameType type);

// Interactive frames are always drained before bulk ones (§ 6.4): a keystroke must
// never wait behind a backfill.
enum class Lane : std::uint8_t {
  Interactive = 0,
  Bulk = 1,
};

struct Frame {
  FrameType type = FrameType::Notification;
  Lane lane = Lane::Interactive;
  std::uint64_t id = 0;
  std::string payload;
};

// Append one encoded frame to `out`. False (and `out` untouched) when the payload
// is over the ceiling: the caller chunks content, and a control body that large is
// a bug, not something to put on the wire.
bool AppendFrame(std::string& out, FrameType type, Lane lane, std::uint64_t id,
                 std::string_view payload);

// Incremental decoder over a byte stream. Bytes arrive in whatever pieces the
// transport read them; complete frames come out in order.
//
// Hostile input by construction (the server's stdin, the client's socket): the
// header is validated BEFORE anything is buffered beyond it — a length over the
// ceiling, an unknown type or lane fails at once — and the announced length is
// never used to size an allocation, so a forged header cannot make the receiver
// reserve 4 GiB. A failure is terminal: the stream has lost framing and the only
// safe thing is to close it.
class FrameDecoder {
 public:
  enum class Error {
    None,
    PayloadTooLarge,
    UnknownType,
    UnknownLane,
  };

  // Append bytes read from the stream. Ignored after an error.
  void Feed(std::string_view bytes);
  // The next complete frame, or nullopt when more bytes are needed (or after an
  // error; check error()).
  std::optional<Frame> Next();

  Error error() const { return error_; }
  // Bytes buffered but not yet part of a returned frame.
  std::size_t buffered_bytes() const { return buffer_.size() - read_offset_; }

 private:
  std::string buffer_;
  std::size_t read_offset_ = 0;
  Error error_ = Error::None;
};

std::string_view FrameDecoderErrorText(FrameDecoder::Error error);

}  // namespace microide::project::remote
