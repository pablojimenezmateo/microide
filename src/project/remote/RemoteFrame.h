#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace microide::project::remote {

inline constexpr std::size_t kFrameHeaderBytes = 4 + 2 + 1 + 8;
inline constexpr std::uint32_t kMaxFramePayloadBytes = 64u * 1024u * 1024u;

enum class FrameType : std::uint16_t {
  Request = 1,
  Response = 2,
  Notification = 3,
  Ack = 4,
  ProcStdin = 16,
  ProcStdout = 17,
  ProcStderr = 18,
  TermInput = 19,
  TermFrame = 20,
  TreeRows = 21,
  ObjectData = 22,
  WriteData = 23,
  WatchDelta = 24,
};

bool IsKnownFrameType(std::uint16_t type);
bool IsContentFrameType(FrameType type);

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

bool AppendFrame(std::string& out, FrameType type, Lane lane, std::uint64_t id,
                 std::string_view payload);

class FrameDecoder {
 public:
  enum class Error {
    None,
    PayloadTooLarge,
    UnknownType,
    UnknownLane,
  };

  void Feed(std::string_view bytes);
  std::optional<Frame> Next();

  Error error() const { return error_; }
  std::size_t buffered_bytes() const { return buffer_.size() - read_offset_; }

 private:
  std::string buffer_;
  std::size_t read_offset_ = 0;
  Error error_ = Error::None;
};

std::string_view FrameDecoderErrorText(FrameDecoder::Error error);

}
