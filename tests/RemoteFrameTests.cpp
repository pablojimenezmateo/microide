#include "TestSupport.h"

#include "project/remote/RemoteFrame.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace microide::tests {
namespace {

using project::remote::AppendFrame;
using project::remote::Frame;
using project::remote::FrameDecoder;
using project::remote::FrameType;
using project::remote::kFrameHeaderBytes;
using project::remote::kMaxFramePayloadBytes;
using project::remote::Lane;

std::vector<Frame> DecodeAll(FrameDecoder& decoder) {
  std::vector<Frame> frames;
  while (std::optional<Frame> frame = decoder.Next()) {
    frames.push_back(std::move(*frame));
  }
  return frames;
}

// Every frame type survives a round trip with its lane, id and payload intact —
// including an empty payload, a payload with NULs, and the largest id.
void TestEveryFrameTypeRoundTrips() {
  const std::vector<FrameType> types = {FrameType::Request,   FrameType::Response,
                                        FrameType::Notification, FrameType::ProcStdin,
                                        FrameType::ProcStdout, FrameType::ProcStderr};
  std::string wire;
  std::vector<std::string> payloads;
  for (std::size_t i = 0; i < types.size(); ++i) {
    std::string payload = i == 0 ? std::string() : std::string("body\0with nul ", 14) + std::to_string(i);
    payloads.push_back(payload);
    Expect(AppendFrame(wire, types[i], i % 2 == 0 ? Lane::Interactive : Lane::Bulk,
                       i == 5 ? UINT64_MAX : i * 1000 + 7, payload),
           "encoding succeeds");
  }
  FrameDecoder decoder;
  decoder.Feed(wire);
  const std::vector<Frame> frames = DecodeAll(decoder);
  Expect(frames.size() == types.size(), "every frame decodes");
  for (std::size_t i = 0; i < frames.size(); ++i) {
    Expect(frames[i].type == types[i], "type round-trips");
    Expect(frames[i].lane == (i % 2 == 0 ? Lane::Interactive : Lane::Bulk), "lane round-trips");
    Expect(frames[i].id == (i == 5 ? UINT64_MAX : i * 1000 + 7), "id round-trips");
    Expect(frames[i].payload == payloads[i], "payload round-trips byte for byte");
  }
  Expect(decoder.error() == FrameDecoder::Error::None && decoder.buffered_bytes() == 0,
         "nothing left over, no error");
}

// The stream arrives in whatever pieces the reads returned — here one byte at a
// time — and decodes to the same frames.
void TestByteAtATimeFeedingDecodesTheSameFrames() {
  std::string wire;
  for (int i = 0; i < 20; ++i) {
    AppendFrame(wire, FrameType::ProcStdout, Lane::Interactive, 3, std::string(i * 13, 'a' + i));
  }
  FrameDecoder decoder;
  std::vector<Frame> frames;
  for (const char byte : wire) {
    decoder.Feed(std::string_view(&byte, 1));
    for (Frame& frame : DecodeAll(decoder)) {
      frames.push_back(std::move(frame));
    }
  }
  Expect(frames.size() == 20, "all twenty frames decode");
  for (int i = 0; i < 20; ++i) {
    Expect(frames[static_cast<std::size_t>(i)].payload == std::string(i * 13, 'a' + i),
           "each payload is intact");
  }
}

// A multi-byte UTF-8 character split across two stdout frames reassembles exactly:
// content frames carry bytes, so neither half is "repaired" into U+FFFD.
void TestSplitUtf8ReassemblesAcrossContentFrames() {
  const std::string text = "ok \xE2\x82\xAC done \xF0\x9F\x98\x80\n";  // € and 😀
  const std::size_t cut = 4;  // inside the euro sign
  std::string wire;
  AppendFrame(wire, FrameType::ProcStdout, Lane::Interactive, 9, text.substr(0, cut));
  AppendFrame(wire, FrameType::ProcStdout, Lane::Interactive, 9, text.substr(cut, 10));
  AppendFrame(wire, FrameType::ProcStdout, Lane::Interactive, 9, text.substr(cut + 10));
  FrameDecoder decoder;
  decoder.Feed(wire);
  std::string reassembled;
  for (const Frame& frame : DecodeAll(decoder)) {
    reassembled += frame.payload;
  }
  Expect(reassembled == text, "the reassembled stream is byte-identical");
  Expect(reassembled.find("\xEF\xBF\xBD") == std::string::npos, "no replacement character");
}

std::string Header(std::uint32_t length, std::uint16_t type, std::uint8_t lane, std::uint64_t id) {
  std::string header;
  for (int i = 0; i < 4; ++i) header.push_back(static_cast<char>((length >> (8 * i)) & 0xff));
  for (int i = 0; i < 2; ++i) header.push_back(static_cast<char>((type >> (8 * i)) & 0xff));
  header.push_back(static_cast<char>(lane));
  for (int i = 0; i < 8; ++i) header.push_back(static_cast<char>((id >> (8 * i)) & 0xff));
  return header;
}

// A header announcing more than the ceiling fails as soon as the header is in,
// without waiting for — or buffering room for — the announced payload.
void TestOversizedFrameFailsAtTheHeader() {
  FrameDecoder decoder;
  decoder.Feed(Header(kMaxFramePayloadBytes + 1, 1, 0, 1));
  Expect(!decoder.Next().has_value(), "no frame");
  Expect(decoder.error() == FrameDecoder::Error::PayloadTooLarge, "the error names the size");
  Expect(decoder.buffered_bytes() == kFrameHeaderBytes, "nothing beyond the header was buffered");
  decoder.Feed(std::string(1024, 'x'));
  Expect(decoder.buffered_bytes() == kFrameHeaderBytes, "and nothing is accepted after the error");

  std::string too_big(kMaxFramePayloadBytes + 1, 'x');
  std::string wire;
  Expect(!AppendFrame(wire, FrameType::ProcStdout, Lane::Bulk, 1, too_big) && wire.empty(),
         "the encoder refuses to put an oversized payload on the wire");
  FrameDecoder at_ceiling;
  at_ceiling.Feed(Header(kMaxFramePayloadBytes, 17, 1, 1));
  Expect(!at_ceiling.Next().has_value() && at_ceiling.error() == FrameDecoder::Error::None,
         "exactly the ceiling is legal (it just waits for the bytes)");
}

void TestUnknownTypeAndLaneFail() {
  FrameDecoder type_decoder;
  type_decoder.Feed(Header(0, 999, 0, 1));
  Expect(!type_decoder.Next().has_value() &&
             type_decoder.error() == FrameDecoder::Error::UnknownType,
         "an unknown type ends the stream");
  FrameDecoder lane_decoder;
  lane_decoder.Feed(Header(0, 1, 7, 1));
  Expect(!lane_decoder.Next().has_value() &&
             lane_decoder.error() == FrameDecoder::Error::UnknownLane,
         "an unknown lane ends the stream");
}

// A truncated frame is not an error, just not a frame yet — the rest may still
// arrive. The earlier complete frames are delivered.
void TestTruncatedFrameWaitsForTheRest() {
  std::string wire;
  AppendFrame(wire, FrameType::Request, Lane::Interactive, 1, "{\"method\":\"a\"}");
  AppendFrame(wire, FrameType::Request, Lane::Interactive, 2, "{\"method\":\"b\"}");
  FrameDecoder decoder;
  decoder.Feed(std::string_view(wire).substr(0, wire.size() - 3));
  const std::vector<Frame> first = DecodeAll(decoder);
  Expect(first.size() == 1 && first[0].id == 1, "the complete frame is delivered");
  Expect(decoder.error() == FrameDecoder::Error::None, "a short read is not an error");
  decoder.Feed(std::string_view(wire).substr(wire.size() - 3));
  const std::vector<Frame> second = DecodeAll(decoder);
  Expect(second.size() == 1 && second[0].id == 2, "the rest completes the second frame");
}

// A long-lived stream does not keep every frame it ever decoded.
void TestDecoderReclaimsConsumedBytes() {
  FrameDecoder decoder;
  std::string frame;
  AppendFrame(frame, FrameType::ProcStdout, Lane::Bulk, 1, std::string(4096, 'z'));
  for (int i = 0; i < 1000; ++i) {
    decoder.Feed(std::string_view(frame).substr(0, 100));
    decoder.Feed(std::string_view(frame).substr(100));
    Expect(DecodeAll(decoder).size() == 1, "one frame per round");
  }
  Expect(decoder.buffered_bytes() == 0, "nothing retained");
}

}  // namespace

void RegisterRemoteFrameTests(std::vector<TestCase>& tests) {
  AddTest(tests, "RemoteFrame/EveryFrameTypeRoundTrips", TestEveryFrameTypeRoundTrips);
  AddTest(tests, "RemoteFrame/ByteAtATimeFeedingDecodesTheSameFrames",
          TestByteAtATimeFeedingDecodesTheSameFrames);
  AddTest(tests, "RemoteFrame/SplitUtf8ReassemblesAcrossContentFrames",
          TestSplitUtf8ReassemblesAcrossContentFrames);
  AddTest(tests, "RemoteFrame/OversizedFrameFailsAtTheHeader", TestOversizedFrameFailsAtTheHeader);
  AddTest(tests, "RemoteFrame/UnknownTypeAndLaneFail", TestUnknownTypeAndLaneFail);
  AddTest(tests, "RemoteFrame/TruncatedFrameWaitsForTheRest", TestTruncatedFrameWaitsForTheRest);
  AddTest(tests, "RemoteFrame/DecoderReclaimsConsumedBytes", TestDecoderReclaimsConsumedBytes);
}

}  // namespace microide::tests
