#include "TestSupport.h"
#include "support/DelayRelay.h"

#include "project/remote/RemoteFrame.h"
#include "project/remote/RemoteFrameTransport.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace microide::tests {
namespace {

#if defined(__unix__) || defined(__APPLE__)

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

// Collects what a transport delivers, from its I/O thread.
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

// Read whatever a raw (non-transport) peer receives, as frames.
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

// Two transports, one on each end: every frame arrives, in order, intact — and the
// transport-internal acks never surface as frames.
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
  // Within a lane, order is preserved; ids tell us which frame is which.
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

// The design's headline: with 50 MiB of bulk queued, an interactive frame goes out
// next, behind at most the bulk window. And bulk beyond the window waits for acks.
void TestInteractiveOvertakesABulkBacklog() {
  SocketPair pair;
  Inbox inbox;
  RemoteFrameTransport sender;
  RemoteFrameTransport::Options options;
  options.bulk_window_initial = 256 * 1024;
  Expect(sender.Start(pair.a, pair.a, inbox.Callbacks(), options), "start");
  const std::string chunk(RemoteFrameTransport::kMaxBulkChunkBytes, 'b');
  for (int i = 0; i < 800; ++i) {  // 50 MiB
    Expect(sender.Send(FrameType::ProcStdout, Lane::Bulk, 1, chunk), "queue bulk");
  }
  Expect(sender.Send(FrameType::Notification, Lane::Interactive, 42, "keystroke"),
         "queue the keystroke");

  FrameDecoder raw;
  std::vector<Frame> received;
  // Read until the keystroke shows up.
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

  // Nothing further flows until the peer acks: the window, not the socket buffer,
  // bounds what is in flight.
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
  std::string value;
  for (int i = 0; i < 8; ++i) value.push_back(static_cast<char>((bulk_total >> (8 * i)) & 0xff));
  AppendFrame(ack, FrameType::Ack, Lane::Interactive, 0, value);
  WriteAll(pair.b, ack);
  Expect(!ReadRawFrames(pair.b, raw, 1).empty(), "an ack releases the next window");
  Expect(inbox.closes.load() == 0, "and the transport is healthy");
}

// Over a link with a real round trip and bandwidth (DelayRelay: 40 ms, 100
// Mbit/s) the adaptive bulk window settles at the bandwidth-delay product plus
// the 5 ms an echo may wait (TD-2026-10-08-325: it used to aim at 100 ms of
// bandwidth whatever the round trip, which queued ~20 ms in front of every
// keystroke on this link), while the link stays full.
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
  constexpr int kChunks = 200;  // 12.5 MiB: about a second on this link
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < kChunks; ++i) {
    Expect(sender.Send(FrameType::ProcStdout, Lane::Bulk, 1, chunk), "queue bulk");
  }
  Expect(WaitUntil([&] { return receiver_inbox.Count() == kChunks; }, std::chrono::seconds(30),
                   std::chrono::milliseconds(5)),
         "everything arrives");
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  const double throughput = kChunks * static_cast<double>(chunk.size()) / seconds;
  const double window = static_cast<double>(sender.bulk_window());
  const double product = kBytesPerSecond * std::chrono::duration<double>(kRtt).count();
  // The rule, against what the transport itself measured: bandwidth x (base RTT +
  // the allowance), less the one chunk a frame-to-ack sample already contains.
  const double base = std::chrono::duration<double>(sender.base_rtt()).count();
  const double rule = sender.bulk_bandwidth() *
                          (base + std::chrono::duration<double>(
                                      RemoteFrameTransport::kQueueAllowance).count()) -
                      static_cast<double>(RemoteFrameTransport::kMaxBulkChunkBytes);
  Expect(base > 0.0 && std::abs(window - std::clamp(rule, 64.0 * 1024, 1024.0 * 1024)) < window * 0.005,
         "the window follows bandwidth x (base RTT + allowance): window " + std::to_string(window) +
             ", rule " + std::to_string(rule));
  // The absolute bound holds only where the simulated link behaves like one: not
  // in a sanitizer build (the relay itself cannot pace 100 Mbit/s there), and not
  // when a loaded box starved the relay's threads (every sample inflated).
  if (!IsSanitizerBuild() && base < 2 * std::chrono::duration<double>(kRtt).count()) {
    Expect(throughput > kBytesPerSecond * 0.8,
           "the link stays full: " + std::to_string(throughput / 1e6) + " MB/s");
    // Below by a measurement's worth (2%): the window comes from the MEASURED
    // bandwidth, which a busy box reads a fraction low -- 499392 against 500000
    // failed a full parallel ctest run once while the rule above held exactly.
    Expect(window >= product * 0.98 && window <= product + kBytesPerSecond * 0.015,
           "the window is the bandwidth-delay product plus a few ms of queue: window " +
               std::to_string(window) + ", product " + std::to_string(product));
  }
}

// On a long, slower link (150 ms, 25 Mbit/s: a product of 469 KB, above the
// 256 KiB the window starts at) the window must GROW to fill it. A window-limited
// link measures window / RTT, so a target computed from that measurement has to
// stay above the current window or the window can never climb out of its start.
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
  constexpr int kChunks = 160;  // 10 MiB: ~3.4 s on this link
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < kChunks; ++i) {
    Expect(sender.Send(FrameType::ProcStdout, Lane::Bulk, 1, chunk), "queue bulk");
  }
  Expect(WaitUntil([&] { return receiver_inbox.Count() == kChunks; }, std::chrono::seconds(60),
                   std::chrono::milliseconds(5)),
         "everything arrives");
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  const double throughput = kChunks * static_cast<double>(chunk.size()) / seconds;
  const double product = kBytesPerSecond * std::chrono::duration<double>(kRtt).count();
  const double window = static_cast<double>(sender.bulk_window());
  // What this test is for: the window climbs out of its start at all.
  Expect(window > static_cast<double>(RemoteFrameTransport::Options{}.bulk_window_initial),
         "the window grew past its start: window " + std::to_string(window));
  // And where it settles is the rule's answer for this link: bandwidth x (RTT +
  // the allowance), less the one chunk a frame-to-ack sample already contains --
  // ~419 KB here. This used to demand 90 % of the bare 469 KB product, i.e. 422 KB,
  // ABOVE the rule's own nominal answer, so it passed only while relay overhead
  // inflated the measured RTT and failed under a loaded parallel ctest (415 KB).
  const double nominal_rule =
      kBytesPerSecond * (std::chrono::duration<double>(kRtt).count() +
                         std::chrono::duration<double>(RemoteFrameTransport::kQueueAllowance).count()) -
      static_cast<double>(RemoteFrameTransport::kMaxBulkChunkBytes);
  Expect(window >= nominal_rule * 0.95,
         "the window grew to the link's product: window " + std::to_string(window) +
             ", rule at the link's nominal rate " + std::to_string(nominal_rule) + ", product " +
             std::to_string(product));
  if (!IsSanitizerBuild()) {
    // Slow start included: a ramp of a few round trips, then a full link.
    Expect(throughput > kBytesPerSecond * 0.7,
           "the link fills: " + std::to_string(throughput / 1e6) + " MB/s of " +
               std::to_string(kBytesPerSecond / 1e6));
  }
}

// Garbage from the peer ends the connection once, with the decoder's reason.
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

// A peer that stops reading is a dead peer: the queue bound tears the connection
// down instead of growing without limit.
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

// An ack for bytes never sent is a lie about the window; it closes the connection.
void TestLyingAckCloses() {
  SocketPair pair;
  Inbox inbox;
  RemoteFrameTransport transport;
  Expect(transport.Start(pair.a, pair.a, inbox.Callbacks()), "start");
  std::string ack;
  AppendFrame(ack, FrameType::Ack, Lane::Interactive, 0, std::string("\x10\x00\x00\x00\x00\x00\x00\x00", 8));
  WriteAll(pair.b, ack);
  Expect(WaitUntil([&]() { return inbox.closes.load() == 1; }), "closed");
  ::close(pair.b);
}

// Flush waits until what was queued has been written — the reply a deliberate
// teardown sends last (server/shutdown's) — and gives up at its deadline when the
// peer does not read.
void TestFlushWaitsForQueuedFrames() {
  SocketPair pair;
  Inbox inbox;
  RemoteFrameTransport transport;
  Expect(transport.Start(pair.a, pair.a, inbox.Callbacks()), "start");
  const std::string big(4 * 1024 * 1024, 'f');  // more than the socket buffers hold
  Expect(transport.Send(FrameType::ProcStdout, Lane::Interactive, 1, big), "queued");
  Expect(!transport.Flush(std::chrono::milliseconds(50)),
         "with nobody reading, the flush gives up at its deadline");
  FrameDecoder decoder;
  std::vector<Frame> frames;
  std::thread reader([&]() { frames = ReadRawFrames(pair.b, decoder, 1); });
  Expect(transport.Flush(std::chrono::seconds(10)), "once the peer reads, it completes");
  reader.join();
  Expect(frames.size() == 1 && frames[0].payload.size() == big.size(), "and the frame arrived whole");
  transport.Stop();
  ::close(pair.b);
}

#endif

}  // namespace

void RegisterRemoteFrameTransportTests(std::vector<TestCase>& tests) {
#if defined(__unix__) || defined(__APPLE__)
  AddTest(tests, "RemoteFrameTransport/FramesRoundTripBetweenTwoTransports",
          TestFramesRoundTripBetweenTwoTransports);
  AddTest(tests, "RemoteFrameTransport/InteractiveOvertakesABulkBacklog",
          TestInteractiveOvertakesABulkBacklog);
  AddTest(tests, "RemoteFrameTransport/BulkWindowSettlesAtTheBandwidthDelayProduct",
          TestBulkWindowSettlesAtTheBandwidthDelayProduct);
  AddTest(tests, "RemoteFrameTransport/BulkWindowFillsALongLink", TestBulkWindowFillsALongLink);
  AddTest(tests, "RemoteFrameTransport/GarbageFromThePeerClosesOnce",
          TestGarbageFromThePeerClosesOnce);
  AddTest(tests, "RemoteFrameTransport/PeerHangupCloses", TestPeerHangupCloses);
  AddTest(tests, "RemoteFrameTransport/WedgedPeerIsTornDown", TestWedgedPeerIsTornDown);
  AddTest(tests, "RemoteFrameTransport/FlushWaitsForQueuedFrames", TestFlushWaitsForQueuedFrames);
  AddTest(tests, "RemoteFrameTransport/LyingAckCloses", TestLyingAckCloses);
#else
  (void)tests;
#endif
}

}  // namespace microide::tests
