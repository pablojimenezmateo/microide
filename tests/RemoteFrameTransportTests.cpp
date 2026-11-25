#include "TestSupport.h"
#include "support/DelayRelay.h"

#include "project/remote/RemoteFrame.h"
#include "project/remote/RemoteFrameTransport.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace microide::tests {
namespace {

using project::remote::AppendFrame;
using project::remote::Frame;
using project::remote::FrameDecoder;
using project::remote::FrameType;
using project::remote::Lane;
using project::remote::RemoteFrameTransport;

struct SocketPair {
  int a = -1;
  int b = -1;
  SocketPair() {
    int fds[2] = {-1, -1};
    Expect(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0, "socketpair");
    a = fds[0];
    b = fds[1];
  }
};

struct Inbox {
  std::mutex mutex;
  std::vector<Frame> frames;
  std::atomic<int> closes{0};
  std::string close_reason;

  RemoteFrameTransport::Callbacks Callbacks() {
    return RemoteFrameTransport::Callbacks{
        .on_frame =
            [this](Frame frame) {
              std::lock_guard lock(mutex);
              frames.push_back(std::move(frame));
            },
        .on_closed =
            [this](std::string_view reason) {
              {
                std::lock_guard lock(mutex);
                close_reason = std::string(reason);
              }
              closes.fetch_add(1);
            },
    };
  }
  std::size_t Count() {
    std::lock_guard lock(mutex);
    return frames.size();
  }
};

std::vector<Frame> ReadRawFrames(int fd, FrameDecoder& decoder, std::size_t want,
                                 std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
  std::vector<Frame> frames;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (frames.size() < want && std::chrono::steady_clock::now() < deadline) {
    pollfd pfd{fd, POLLIN, 0};
    if (::poll(&pfd, 1, 50) <= 0) {
      continue;
    }
    char buffer[65536];
    const ssize_t n = ::read(fd, buffer, sizeof(buffer));
    if (n <= 0) {
      break;
    }
    decoder.Feed(std::string_view(buffer, static_cast<std::size_t>(n)));
    while (std::optional<Frame> frame = decoder.Next()) {
      frames.push_back(std::move(*frame));
    }
  }
  return frames;
}

std::string AckPayload(std::uint64_t bytes, std::uint64_t received_us) {
  std::string payload;
  for (const std::uint64_t value : {bytes, received_us}) {
    for (int i = 0; i < 8; ++i) {
      payload.push_back(static_cast<char>((value >> (8 * i)) & 0xffu));
    }
  }
  return payload;
}

constexpr int ChunksFor(double bytes) {
  return static_cast<int>(bytes / RemoteFrameTransport::kMaxBulkChunkBytes);
}

void WriteAll(int fd, const std::string& bytes) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const ssize_t n = ::write(fd, bytes.data() + offset, bytes.size() - offset);
    if (n > 0) {
      offset += static_cast<std::size_t>(n);
    } else if (n < 0 && errno != EINTR && errno != EAGAIN) {
      break;
    }
  }
}

void TestFramesRoundTripBetweenTwoTransports() {
  SocketPair pair;
  Inbox inbox_a;
  Inbox inbox_b;
  RemoteFrameTransport a;
  RemoteFrameTransport b;
  Expect(a.Start(pair.a, pair.a, inbox_a.Callbacks()), "start a");
  Expect(b.Start(pair.b, pair.b, inbox_b.Callbacks()), "start b");
  for (int i = 0; i < 200; ++i) {
    const Lane lane = i % 3 == 0 ? Lane::Bulk : Lane::Interactive;
    Expect(a.Send(i % 2 == 0 ? FrameType::ProcStdout : FrameType::Notification, lane,
                  static_cast<std::uint64_t>(i), std::string(static_cast<std::size_t>(i) * 97, 'q')),
           "send");
  }
  Expect(WaitUntil([&]() { return inbox_b.Count() == 200; }, std::chrono::seconds(5)),
         "every frame arrives");
  std::lock_guard lock(inbox_b.mutex);
  std::uint64_t last_interactive = 0;
  std::uint64_t last_bulk = 0;
  bool first_interactive = true;
  bool first_bulk = true;
  for (const Frame& frame : inbox_b.frames) {
    Expect(frame.type != FrameType::Ack, "acks are transport-internal");
    Expect(frame.payload == std::string(static_cast<std::size_t>(frame.id) * 97, 'q'),
           "payload intact");
    std::uint64_t& last = frame.lane == Lane::Bulk ? last_bulk : last_interactive;
    bool& first = frame.lane == Lane::Bulk ? first_bulk : first_interactive;
    Expect(first || frame.id > last, "per-lane order is preserved");
    first = false;
    last = frame.id;
  }
  Expect(inbox_a.closes.load() == 0 && inbox_b.closes.load() == 0, "nobody closed");
}

void TestInteractiveOvertakesABulkBacklog() {
  SocketPair pair;
  Inbox inbox;
  RemoteFrameTransport sender;
  RemoteFrameTransport::Options options;
  options.bulk_window_initial = 256 * 1024;
  Expect(sender.Start(pair.a, pair.a, inbox.Callbacks(), options), "start");
  const std::string chunk(RemoteFrameTransport::kMaxBulkChunkBytes, 'b');
  for (int i = 0; i < 800; ++i) {
    Expect(sender.Send(FrameType::ProcStdout, Lane::Bulk, 1, chunk), "queue bulk");
  }
  Expect(sender.Send(FrameType::Notification, Lane::Interactive, 42, "keystroke"),
         "queue the keystroke");

  FrameDecoder raw;
  std::vector<Frame> received;
  while (true) {
    std::vector<Frame> more = ReadRawFrames(pair.b, raw, 1, std::chrono::seconds(5));
    Expect(!more.empty(), "frames keep arriving until the keystroke");
    bool found = false;
    for (Frame& frame : more) {
      found = found || frame.id == 42;
      received.push_back(std::move(frame));
    }
    if (found) {
      break;
    }
  }
  std::size_t bulk_before = 0;
  for (const Frame& frame : received) {
    if (frame.id == 42) {
      break;
    }
    bulk_before += frame.payload.size();
  }
  Expect(bulk_before <= options.bulk_window_initial + RemoteFrameTransport::kMaxBulkChunkBytes,
         "at most one window of bulk went ahead of the keystroke (" + std::to_string(bulk_before) +
             " bytes)");

  std::size_t bulk_total = 0;
  for (const Frame& frame : received) {
    bulk_total += frame.id == 42 ? 0 : frame.payload.size();
  }
  const std::vector<Frame> stalled = ReadRawFrames(pair.b, raw, 1, std::chrono::milliseconds(200));
  for (const Frame& frame : stalled) {
    bulk_total += frame.payload.size();
  }
  Expect(bulk_total <= options.bulk_window_initial + RemoteFrameTransport::kMaxBulkChunkBytes,
         "bulk stops at the window without acks");

  std::string ack;
  AppendFrame(ack, FrameType::Ack, Lane::Interactive, 0, AckPayload(bulk_total, 1));
  WriteAll(pair.b, ack);
  Expect(!ReadRawFrames(pair.b, raw, 1).empty(), "an ack releases the next window");
  Expect(inbox.closes.load() == 0, "and the transport is healthy");
}

void TestBulkWindowSettlesAtTheBandwidthDelayProduct() {
  SocketPair pair;
  constexpr auto kRtt = std::chrono::milliseconds(40);
  constexpr double kBytesPerSecond = 12'500'000.0;
  DelayRelay relay(pair.b, kRtt, static_cast<std::uint64_t>(kBytesPerSecond));
  Inbox sender_inbox;
  Inbox receiver_inbox;
  RemoteFrameTransport sender;
  RemoteFrameTransport receiver;
  Expect(sender.Start(pair.a, pair.a, sender_inbox.Callbacks(), {}), "start the sender");
  const int far = relay.TakeClientFd();
  Expect(receiver.Start(far, far, receiver_inbox.Callbacks(), {}), "start the receiver");
  const std::string chunk(RemoteFrameTransport::kMaxBulkChunkBytes, 'b');
  constexpr int kChunks = ChunksFor(12.5 * 1024 * 1024);
  const TimingWindow timing;
  for (int i = 0; i < kChunks; ++i) {
    Expect(sender.Send(FrameType::ProcStdout, Lane::Bulk, 1, chunk), "queue bulk");
  }
  Expect(WaitUntil([&] { return receiver_inbox.Count() == kChunks; }, std::chrono::seconds(30),
                   std::chrono::milliseconds(5)),
         "everything arrives");
  const double seconds = std::chrono::duration<double>(timing.Elapsed()).count();
  const double throughput = kChunks * static_cast<double>(chunk.size()) / seconds;
  const double window = static_cast<double>(sender.bulk_window());
  const double product = kBytesPerSecond * std::chrono::duration<double>(kRtt).count();
  const double base = std::chrono::duration<double>(sender.base_rtt()).count();
  const double rule = RemoteFrameTransport::BulkWindowTarget(sender.bulk_bandwidth(), sender.base_rtt());
  Expect(base > 0.0 && std::abs(window - std::clamp(rule, 64.0 * 1024, 1024.0 * 1024)) < window * 0.005,
         "the window follows bandwidth x (base RTT + allowance): window " + std::to_string(window) +
             ", rule " + std::to_string(rule));
  if (timing.Trustworthy() && base < 2 * std::chrono::duration<double>(kRtt).count()) {
    Expect(throughput > kBytesPerSecond * 0.75,
           "the link stays full: " + std::to_string(throughput / 1e6) + " MB/s");
    Expect(window >= product * 0.98 && window <= product + kBytesPerSecond * 0.015,
           "the window is the bandwidth-delay product plus a few ms of queue: window " +
               std::to_string(window) + ", product " + std::to_string(product));
  }
}

void TestBulkWindowClimbsBackFromItsFloor() {
  SocketPair pair;
  constexpr auto kRtt = std::chrono::milliseconds(40);
  constexpr double kBytesPerSecond = 12'500'000.0;
  DelayRelay relay(pair.b, kRtt, 500'000);
  Inbox sender_inbox;
  Inbox receiver_inbox;
  RemoteFrameTransport sender;
  RemoteFrameTransport receiver;
  Expect(sender.Start(pair.a, pair.a, sender_inbox.Callbacks(), {}), "start the sender");
  const int far = relay.TakeClientFd();
  Expect(receiver.Start(far, far, receiver_inbox.Callbacks(), {}), "start the receiver");
  const std::string chunk(RemoteFrameTransport::kMaxBulkChunkBytes, 'b');
  constexpr int kSlowChunks = ChunksFor(1.5 * 1024 * 1024);
  for (int i = 0; i < kSlowChunks; ++i) {
    Expect(sender.Send(FrameType::ProcStdout, Lane::Bulk, 1, chunk), "queue bulk");
  }
  Expect(WaitUntil([&] { return receiver_inbox.Count() >= kSlowChunks - 2; }, std::chrono::seconds(30),
                   std::chrono::milliseconds(5)),
         "the slow stretch is carried");
  const std::size_t slow_window = sender.bulk_window();
  relay.SetBytesPerSecond(static_cast<std::uint64_t>(kBytesPerSecond));
  constexpr int kFastChunks = ChunksFor(12.5 * 1024 * 1024);
  const TimingWindow timing;
  for (int i = 0; i < kFastChunks; ++i) {
    Expect(sender.Send(FrameType::ProcStdout, Lane::Bulk, 1, chunk), "queue bulk");
  }
  Expect(WaitUntil([&] { return receiver_inbox.Count() == kSlowChunks + kFastChunks; },
                   std::chrono::seconds(60), std::chrono::milliseconds(5)),
         "everything arrives");
  const double seconds = std::chrono::duration<double>(timing.Elapsed()).count();
  const double throughput = kFastChunks * static_cast<double>(chunk.size()) / seconds;
  if (timing.Trustworthy()) {
    Expect(throughput > kBytesPerSecond * 0.5,
           "the window climbs back with the link: " + std::to_string(throughput / 1e6) + " MB/s, window " +
               std::to_string(sender.bulk_window()) + " (it was " + std::to_string(slow_window) +
               " on the slow link)");
  }
}

void TestBulkBandwidthIgnoresCompressedAcks() {
  SocketPair pair;
  constexpr auto kRtt = std::chrono::milliseconds(40);
  constexpr double kBytesPerSecond = 12'500'000.0;
  DelayRelay relay(pair.b, kRtt, static_cast<std::uint64_t>(kBytesPerSecond));
  Inbox sender_inbox;
  Inbox receiver_inbox;
  RemoteFrameTransport sender;
  RemoteFrameTransport receiver;
  Expect(sender.Start(pair.a, pair.a, sender_inbox.Callbacks(), {}), "start the sender");
  const int far = relay.TakeClientFd();
  Expect(receiver.Start(far, far, receiver_inbox.Callbacks(), {}), "start the receiver");
  const std::string chunk(RemoteFrameTransport::kMaxBulkChunkBytes, 'b');
  constexpr int kChunks = ChunksFor(12.5 * 1024 * 1024);
  for (int i = 0; i < kChunks; ++i) {
    Expect(sender.Send(FrameType::ProcStdout, Lane::Bulk, 1, chunk), "queue bulk");
  }
  Expect(WaitUntil([&] { return receiver_inbox.Count() >= kChunks / 4; }, std::chrono::seconds(30),
                   std::chrono::milliseconds(1)),
         "the transfer is under way");
  relay.SetClientStalled(true);
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  relay.SetClientStalled(false);
  double peak = 0.0;
  Expect(WaitUntil(
             [&] {
               peak = std::max(peak, sender.bulk_bandwidth());
               return receiver_inbox.Count() == kChunks;
             },
             std::chrono::seconds(30), std::chrono::milliseconds(1)),
         "everything arrives");
  Expect(peak < kBytesPerSecond * 2,
         "bunched acks do not read as a faster link: peak " + std::to_string(peak / 1e6) + " MB/s");
  Expect(sender.bulk_window() < 1024 * 1024,
         "nor pin the window at its cap: " + std::to_string(sender.bulk_window()));
}

void TestBulkWindowFillsALongLink() {
  SocketPair pair;
  constexpr auto kRtt = std::chrono::milliseconds(150);
  constexpr double kBytesPerSecond = 3'125'000.0;
  DelayRelay relay(pair.b, kRtt, static_cast<std::uint64_t>(kBytesPerSecond));
  Inbox sender_inbox;
  Inbox receiver_inbox;
  RemoteFrameTransport sender;
  RemoteFrameTransport receiver;
  Expect(sender.Start(pair.a, pair.a, sender_inbox.Callbacks(), {}), "start the sender");
  const int far = relay.TakeClientFd();
  Expect(receiver.Start(far, far, receiver_inbox.Callbacks(), {}), "start the receiver");
  const std::string chunk(RemoteFrameTransport::kMaxBulkChunkBytes, 'b');
  constexpr int kChunks = ChunksFor(10.0 * 1024 * 1024);
  const TimingWindow timing;
  for (int i = 0; i < kChunks; ++i) {
    Expect(sender.Send(FrameType::ProcStdout, Lane::Bulk, 1, chunk), "queue bulk");
  }
  Expect(WaitUntil([&] { return receiver_inbox.Count() == kChunks; }, std::chrono::seconds(60),
                   std::chrono::milliseconds(5)),
         "everything arrives");
  const double seconds = std::chrono::duration<double>(timing.Elapsed()).count();
  const double throughput = kChunks * static_cast<double>(chunk.size()) / seconds;
  const double product = kBytesPerSecond * std::chrono::duration<double>(kRtt).count();
  const double window = static_cast<double>(sender.bulk_window());
  const double base = std::chrono::duration<double>(sender.base_rtt()).count();
  const double measured_rule = RemoteFrameTransport::BulkWindowTarget(sender.bulk_bandwidth(), sender.base_rtt());
  Expect(base > 0.0 && std::abs(window - std::clamp(measured_rule, 64.0 * 1024, 1024.0 * 1024)) < window * 0.005,
         "the window follows the measured bandwidth x (base RTT + allowance): window " +
             std::to_string(window) + ", rule " + std::to_string(measured_rule));
  if (timing.StalledFraction() < 0.10) {
    Expect(window > static_cast<double>(RemoteFrameTransport::Options{}.bulk_window_initial),
           "the window grew past its start: window " + std::to_string(window) + "; " + timing.Describe());
  }
  const double nominal_rule = RemoteFrameTransport::BulkWindowTarget(kBytesPerSecond, kRtt);
  if (timing.Trustworthy()) {
    Expect(window >= nominal_rule * 0.80,
           "the window grew to the link's product: window " + std::to_string(window) +
               ", rule at the link's nominal rate " + std::to_string(nominal_rule) + ", product " +
               std::to_string(product));
    Expect(throughput > kBytesPerSecond * 0.7,
           "the link fills: " + std::to_string(throughput / 1e6) + " MB/s of " +
               std::to_string(kBytesPerSecond / 1e6));
  }
}

void TestGarbageFromThePeerClosesOnce() {
  SocketPair pair;
  Inbox inbox;
  RemoteFrameTransport transport;
  Expect(transport.Start(pair.a, pair.a, inbox.Callbacks()), "start");
  WriteAll(pair.b, std::string(15, '\xff'));
  Expect(WaitUntil([&]() { return inbox.closes.load() == 1; }), "closed");
  Expect(transport.closed(), "reports closed");
  Expect(!transport.Send(FrameType::Notification, Lane::Interactive, 1, "x"),
         "sending after close fails");
  std::lock_guard lock(inbox.mutex);
  Expect(inbox.close_reason.find("ceiling") != std::string::npos ||
             inbox.close_reason.find("unknown") != std::string::npos,
         "the reason is the protocol error: " + inbox.close_reason);
}

void TestPeerHangupCloses() {
  SocketPair pair;
  Inbox inbox;
  RemoteFrameTransport transport;
  Expect(transport.Start(pair.a, pair.a, inbox.Callbacks()), "start");
  ::close(pair.b);
  Expect(WaitUntil([&]() { return inbox.closes.load() == 1; }), "EOF closes the transport");
}

void TestWedgedPeerIsTornDown() {
  SocketPair pair;
  Inbox inbox;
  RemoteFrameTransport transport;
  RemoteFrameTransport::Options options;
  options.max_queued_bytes = 2 * 1024 * 1024;
  Expect(transport.Start(pair.a, pair.a, inbox.Callbacks(), options), "start");
  const std::string chunk(32 * 1024, 'w');
  bool refused = false;
  for (int i = 0; i < 10000 && !refused; ++i) {
    refused = !transport.Send(FrameType::ProcStdout, Lane::Interactive, 1, chunk);
  }
  Expect(refused, "the queue bound refuses further frames");
  Expect(WaitUntil([&]() { return inbox.closes.load() == 1; }), "and the transport closes");
  ::close(pair.b);
}

void TestLyingAckCloses() {
  SocketPair pair;
  Inbox inbox;
  RemoteFrameTransport transport;
  Expect(transport.Start(pair.a, pair.a, inbox.Callbacks()), "start");
  std::string ack;
  AppendFrame(ack, FrameType::Ack, Lane::Interactive, 0, AckPayload(16, 1));
  WriteAll(pair.b, ack);
  Expect(WaitUntil([&]() { return inbox.closes.load() == 1; }), "closed");
  ::close(pair.b);
}

void TestAnAckWithoutTheReceiveClockCloses() {
  SocketPair pair;
  Inbox inbox;
  RemoteFrameTransport transport;
  Expect(transport.Start(pair.a, pair.a, inbox.Callbacks()), "start");
  std::string ack;
  AppendFrame(ack, FrameType::Ack, Lane::Interactive, 0, AckPayload(0, 0).substr(0, 8));
  WriteAll(pair.b, ack);
  Expect(WaitUntil([&]() { return inbox.closes.load() == 1; }), "closed");
  std::lock_guard lock(inbox.mutex);
  Expect(inbox.close_reason == "malformed ack frame", "as malformed: " + inbox.close_reason);
  ::close(pair.b);
}

void TestFlushWaitsForQueuedFrames() {
  SocketPair pair;
  Inbox inbox;
  RemoteFrameTransport transport;
  Expect(transport.Start(pair.a, pair.a, inbox.Callbacks()), "start");
  const std::string big(4 * 1024 * 1024, 'f');
  Expect(transport.Send(FrameType::ProcStdout, Lane::Interactive, 1, big), "queued");
  Expect(!transport.Flush(std::chrono::milliseconds(50), std::chrono::seconds(10)),
         "with nobody reading, the flush gives up once it has stalled");
  FrameDecoder decoder;
  std::vector<Frame> frames;
  std::thread reader([&]() { frames = ReadRawFrames(pair.b, decoder, 1); });
  Expect(transport.Flush(std::chrono::seconds(10), std::chrono::seconds(10)),
         "once the peer reads, it completes");
  reader.join();
  Expect(frames.size() == 1 && frames[0].payload.size() == big.size(), "and the frame arrived whole");
  transport.Stop();
  ::close(pair.b);
}

void TestFlushWaitsForASlowReaderUpToItsLimit() {
  const std::string big(4 * 1024 * 1024, 'f');
  const auto trickle = [](int fd, std::atomic<bool>& stop) {
    std::vector<char> chunk(64 * 1024);
    while (!stop.load()) {
      if (::read(fd, chunk.data(), chunk.size()) <= 0) {
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  };
  {
    SocketPair pair;
    Inbox inbox;
    RemoteFrameTransport transport;
    Expect(transport.Start(pair.a, pair.a, inbox.Callbacks()), "start");
    Expect(transport.Send(FrameType::ProcStdout, Lane::Interactive, 1, big), "queued");
    std::atomic<bool> stop{false};
    std::thread reader(trickle, pair.b, std::ref(stop));
    const auto start = std::chrono::steady_clock::now();
    const bool flushed = transport.Flush(std::chrono::milliseconds(300), std::chrono::seconds(30));
    const auto took = std::chrono::steady_clock::now() - start;
    Expect(flushed, "a reader that keeps reading gets all of it");
    Expect(took > std::chrono::milliseconds(300), "which took longer than the stall time");
    stop = true;
    transport.Stop();
    ::shutdown(pair.b, SHUT_RDWR);
    reader.join();
    ::close(pair.b);
  }
  {
    SocketPair pair;
    Inbox inbox;
    RemoteFrameTransport transport;
    Expect(transport.Start(pair.a, pair.a, inbox.Callbacks()), "start");
    Expect(transport.Send(FrameType::ProcStdout, Lane::Interactive, 1, big), "queued");
    std::atomic<bool> stop{false};
    std::thread reader(trickle, pair.b, std::ref(stop));
    Expect(!transport.Flush(std::chrono::seconds(10), std::chrono::milliseconds(100)),
           "and the limit still ends it");
    stop = true;
    transport.Stop();
    ::shutdown(pair.b, SHUT_RDWR);
    reader.join();
    ::close(pair.b);
  }
}

}

void RegisterRemoteFrameTransportTests(std::vector<TestCase>& tests) {
  AddTest(tests, "RemoteFrameTransport/FramesRoundTripBetweenTwoTransports",
          TestFramesRoundTripBetweenTwoTransports);
  AddTest(tests, "RemoteFrameTransport/InteractiveOvertakesABulkBacklog",
          TestInteractiveOvertakesABulkBacklog);
  AddTest(tests, "RemoteFrameTransport/BulkWindowSettlesAtTheBandwidthDelayProduct",
          TestBulkWindowSettlesAtTheBandwidthDelayProduct);
  AddTest(tests, "RemoteFrameTransport/BulkWindowFillsALongLink", TestBulkWindowFillsALongLink);
  AddTest(tests, "RemoteFrameTransport/BulkWindowClimbsBackFromItsFloor", TestBulkWindowClimbsBackFromItsFloor);
  AddTest(tests, "RemoteFrameTransport/BulkBandwidthIgnoresCompressedAcks", TestBulkBandwidthIgnoresCompressedAcks);
  AddTest(tests, "RemoteFrameTransport/AnAckWithoutTheReceiveClockCloses", TestAnAckWithoutTheReceiveClockCloses);
  AddTest(tests, "RemoteFrameTransport/GarbageFromThePeerClosesOnce",
          TestGarbageFromThePeerClosesOnce);
  AddTest(tests, "RemoteFrameTransport/PeerHangupCloses", TestPeerHangupCloses);
  AddTest(tests, "RemoteFrameTransport/WedgedPeerIsTornDown", TestWedgedPeerIsTornDown);
  AddTest(tests, "RemoteFrameTransport/FlushWaitsForQueuedFrames", TestFlushWaitsForQueuedFrames);
  AddTest(tests, "RemoteFrameTransport/FlushWaitsForASlowReaderUpToItsLimit",
          TestFlushWaitsForASlowReaderUpToItsLimit);
  AddTest(tests, "RemoteFrameTransport/LyingAckCloses", TestLyingAckCloses);
}

}
