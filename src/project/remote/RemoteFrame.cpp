#include "project/remote/RemoteFrame.h"

#include "util/ByteCodec.h"

namespace microide::project::remote {
using util::GetLe;
using util::PutLe;

bool IsKnownFrameType(std::uint16_t type) {
  switch (static_cast<FrameType>(type)) {
    case FrameType::Request:
    case FrameType::Response:
    case FrameType::Notification:
    case FrameType::Ack:
    case FrameType::ProcStdin:
    case FrameType::ProcStdout:
    case FrameType::ProcStderr:
    case FrameType::TermInput:
    case FrameType::TermFrame:
      return true;
  }
  return false;
}

bool IsContentFrameType(FrameType type) {
  return static_cast<std::uint16_t>(type) >= 16;
}

bool AppendFrame(std::string& out, FrameType type, Lane lane, std::uint64_t id,
                 std::string_view payload) {
  if (payload.size() > kMaxFramePayloadBytes) {
    return false;
  }
  out.reserve(out.size() + kFrameHeaderBytes + payload.size());
  PutLe(out, payload.size(), 4);
  PutLe(out, static_cast<std::uint16_t>(type), 2);
  PutLe(out, static_cast<std::uint8_t>(lane), 1);
  PutLe(out, id, 8);
  out.append(payload);
  return true;
}

void FrameDecoder::Feed(std::string_view bytes) {
  if (error_ != Error::None || bytes.empty()) {
    return;
  }
  // Reclaim consumed bytes before growing: a long-lived stream would otherwise keep
  // every frame it ever decoded. Compacting only when the dead prefix is at least
  // half the buffer keeps it amortized O(1) per byte.
  if (read_offset_ > 0 && read_offset_ * 2 >= buffer_.size()) {
    buffer_.erase(0, read_offset_);
    read_offset_ = 0;
  }
  buffer_.append(bytes);
}

std::optional<Frame> FrameDecoder::Next() {
  if (error_ != Error::None) {
    return std::nullopt;
  }
  const std::string_view pending(buffer_.data() + read_offset_, buffer_.size() - read_offset_);
  if (pending.size() < kFrameHeaderBytes) {
    return std::nullopt;
  }
  const std::uint64_t length = GetLe(pending, 0, 4);
  const std::uint64_t type = GetLe(pending, 4, 2);
  const std::uint64_t lane = GetLe(pending, 6, 1);
  // Validate the header as soon as it is complete, before waiting for (or
  // buffering) its payload: a forged length must fail now, not after the peer has
  // been allowed to stream 64 MiB at us.
  if (length > kMaxFramePayloadBytes) {
    error_ = Error::PayloadTooLarge;
    return std::nullopt;
  }
  if (!IsKnownFrameType(static_cast<std::uint16_t>(type))) {
    error_ = Error::UnknownType;
    return std::nullopt;
  }
  if (lane > static_cast<std::uint8_t>(Lane::Bulk)) {
    error_ = Error::UnknownLane;
    return std::nullopt;
  }
  if (pending.size() - kFrameHeaderBytes < length) {
    return std::nullopt;
  }
  Frame frame{
      .type = static_cast<FrameType>(type),
      .lane = static_cast<Lane>(lane),
      .id = GetLe(pending, 7, 8),
      .payload = std::string(pending.substr(kFrameHeaderBytes, static_cast<std::size_t>(length))),
  };
  read_offset_ += kFrameHeaderBytes + static_cast<std::size_t>(length);
  if (read_offset_ == buffer_.size()) {
    buffer_.clear();
    read_offset_ = 0;
  }
  return frame;
}

std::string_view FrameDecoderErrorText(FrameDecoder::Error error) {
  switch (error) {
    case FrameDecoder::Error::None:
      return "no error";
    case FrameDecoder::Error::PayloadTooLarge:
      return "frame announces a payload over the 64 MiB ceiling";
    case FrameDecoder::Error::UnknownType:
      return "frame of an unknown type";
    case FrameDecoder::Error::UnknownLane:
      return "frame on an unknown lane";
  }
  return "unknown error";
}

}  // namespace microide::project::remote
