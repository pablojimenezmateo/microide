#include "TestSupport.h"

#include "project/remote/LinkHeartbeat.h"
#include "project/remote/RemoteFrame.h"
#include "project/remote/RemotePeer.h"
#include "project/remote/RemoteProtocol.h"
#include "util/JsonValue.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace microide::tests {
namespace {

using project::remote::LinkHeartbeat;
namespace remote = project::remote;

// Three unanswered pings declare the link dead at the moment the fourth would go
// out: 6 s at the 2 s default — no waiting, made-up time.
void TestHeartbeatDeclaresDeathAfterThreeMisses() {
  LinkHeartbeat heartbeat;
  Expect(heartbeat.Tick(0) == 0, "the first ping goes at once");
  Expect(!heartbeat.Tick(1999).has_value(), "nothing before the interval");
  Expect(heartbeat.Tick(2000) == 2000 && heartbeat.Tick(4000) == 4000, "one ping per interval");
  Expect(!heartbeat.dead(), "two seconds into the third miss it is not dead yet");
  Expect(!heartbeat.Tick(6000).has_value() && heartbeat.dead(),
         "three misses: dead at 6 s, no fourth ping");
}

void TestHeartbeatAnswerResetsMissesAndMeasuresRtt() {
  LinkHeartbeat heartbeat;
  (void)heartbeat.Tick(0);
  (void)heartbeat.Tick(2000);
  (void)heartbeat.Tick(4000);
  heartbeat.OnPong(4000, 4080);
  Expect(heartbeat.outstanding() == 0 && heartbeat.rtt_ms() == 80, "an answer clears the misses");
  (void)heartbeat.Tick(6000);
  Expect(!heartbeat.dead(), "and the link lives");
  heartbeat.OnPong(6000, 6160);
  Expect(heartbeat.rtt_ms() == 80 + (160 - 80) / 8, "the RTT is smoothed, not replaced");
  heartbeat.OnPong(123456, 123500);
  heartbeat.OnPong(-5, 0);
  Expect(heartbeat.rtt_ms() == 90, "stamps it never sent are ignored");
}

void TestHelloRoundTripsAndValidates() {
  remote::HelloReply reply{.release = "2.14.0",
                           .daemon_epoch = "e-1",
                           .capabilities = {"git", "watch"},
                           .session_survival = {.kill_user_processes = true, .linger = false}};
  std::string error;
  const auto decoded = remote::HelloReplyFromJson(
      *util::ParseJson(util::SerializeJson(remote::ToJson(reply))), &error);
  Expect(decoded.has_value() && decoded->release == "2.14.0" && decoded->daemon_epoch == "e-1" &&
             decoded->capabilities.size() == 2 && decoded->session_survival.kill_user_processes,
         "a hello reply round-trips: " + error);

  const auto bad = [](std::string_view json) {
    std::string why;
    return !remote::HelloRequestFromJson(*util::ParseJson(json), &why).has_value() && !why.empty();
  };
  Expect(bad(R"({"protocol":1,"min_protocol":1})"), "a missing release is refused");
  Expect(bad(R"({"protocol":"1","min_protocol":1,"release":"x"})"), "a string version is refused");
  Expect(bad(R"({"protocol":1,"min_protocol":2,"release":"x"})"), "min above protocol is refused");
  Expect(bad(R"({"protocol":0,"min_protocol":0,"release":"x"})"), "version 0 is refused");
  Expect(bad(std::string(R"({"protocol":1,"min_protocol":1,"release":")") + std::string(100, 'x') +
             "\"}"),
         "an oversized release is refused");
  Expect(bad("[]"), "a non-object is refused");
}

void TestCompatibilityNamesBothReleases() {
  Expect(!remote::CheckProtocolCompatibility(3, 2, "2.14", 2, 1, "2.12").has_value(),
         "a newer client and an older server whose wire still fits are compatible");
  const auto too_old = remote::CheckProtocolCompatibility(3, 3, "2.14", 2, 1, "2.12");
  Expect(too_old.has_value() && too_old->find("2.12") != std::string::npos &&
             too_old->find("2.14") != std::string::npos &&
             too_old->find("too old for this microide") != std::string::npos,
         "a server below the client's floor is reported, naming both releases");
  const auto client_old = remote::CheckProtocolCompatibility(1, 1, "2.10", 4, 3, "2.20");
  Expect(client_old.has_value() && client_old->find("this microide 2.10") != std::string::npos,
         "and so is a client below the server's floor");
}

#if defined(__unix__) || defined(__APPLE__)

struct Pair {
  int a = -1;
  int b = -1;
  Pair() {
    int fds[2];
    Expect(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0, "socketpair");
    a = fds[0];
    b = fds[1];
  }
};

struct Result {
  std::mutex mutex;
  std::optional<util::JsonValue> value;
  std::optional<remote::RemotePeer::RpcError> error;
  std::atomic<bool> done{false};
  remote::RemotePeer::ResponseHandler Handler() {
    return [this](std::optional<util::JsonValue> result,
                  std::optional<remote::RemotePeer::RpcError> rpc_error) {
      std::lock_guard lock(mutex);
      value = std::move(result);
      error = std::move(rpc_error);
      done = true;
    };
  }
};

// A request reaches its handler and the reply reaches the caller; an unknown
// method is answered with an error rather than silence.
void TestRequestsAndErrorsRoundTrip() {
  Pair pair;
  remote::RemotePeer server;
  server.OnRequest("echo", [&server](std::uint64_t id, const util::JsonValue& params) {
    server.Reply(id, params);
  });
  remote::RemotePeer client;
  Expect(server.Start(pair.b, pair.b, {}) && client.Start(pair.a, pair.a, {}), "start");

  Result echoed;
  util::JsonObject params;
  params["text"] = util::JsonValue("héllo");
  Expect(client.Request("echo", util::JsonValue(params), remote::Lane::Interactive,
                        echoed.Handler()) != 0,
         "the request is sent");
  Result unknown;
  client.Request("nope", util::JsonValue(nullptr), remote::Lane::Interactive, unknown.Handler());
  Expect(WaitUntil([&]() { return echoed.done.load() && unknown.done.load(); }), "both answered");
  std::lock_guard lock_a(echoed.mutex);
  Expect(echoed.value.has_value() && (*echoed.value)["text"].AsString() == "héllo", "echoed");
  std::lock_guard lock_b(unknown.mutex);
  Expect(unknown.error.has_value() && unknown.error->code == remote::kErrorUnknownMethod,
         "unknown method is an error reply");
}

// A malformed control body gets a protocol error for that request, and the
// connection — and the next request — carry on.
void TestMalformedBodyIsRejectedAndTheConnectionContinues() {
  Pair pair;
  remote::RemotePeer server;
  std::atomic<int> handled{0};
  server.OnRequest("ok", [&](std::uint64_t id, const util::JsonValue&) {
    handled.fetch_add(1);
    server.Reply(id, util::JsonValue(true));
  });
  Expect(server.Start(pair.b, pair.b, {}), "start");
  std::string wire;
  remote::AppendFrame(wire, remote::FrameType::Request, remote::Lane::Interactive, 7, "{not json");
  remote::AppendFrame(wire, remote::FrameType::Request, remote::Lane::Interactive, 8,
                      R"({"method":"ok","params":null})");
  Expect(::write(pair.a, wire.data(), wire.size()) == static_cast<ssize_t>(wire.size()), "write");
  remote::FrameDecoder decoder;
  std::vector<remote::Frame> replies;
  Expect(WaitUntil([&]() {
           char buffer[4096];
           const ssize_t n = ::recv(pair.a, buffer, sizeof(buffer), MSG_DONTWAIT);
           if (n > 0) {
             decoder.Feed(std::string_view(buffer, static_cast<std::size_t>(n)));
             while (auto frame = decoder.Next()) replies.push_back(std::move(*frame));
           }
           return replies.size() >= 2;
         }),
         "both requests are answered");
  Expect(replies[0].id == 7 && util::ParseJson(replies[0].payload)->HasKey("error"),
         "the malformed one gets an error");
  Expect(replies[1].id == 8 && handled.load() == 1, "the next one is handled normally");
  Expect(!server.closed(), "the connection is still up");
  ::close(pair.a);
}

// op/cancel reaches a streaming handler, which stops and sends its terminal reply.
void TestCancelReachesTheHandler() {
  Pair pair;
  remote::RemotePeer server;
  std::atomic<std::uint64_t> held{0};
  std::atomic<std::uint64_t> cancelled{0};
  server.OnRequest("stream", [&](std::uint64_t id, const util::JsonValue&) { held = id; });
  server.OnCancel([&](std::uint64_t id) {
    cancelled = id;
    server.ReplyError(id, remote::kErrorCancelled, "cancelled");
  });
  remote::RemotePeer client;
  Expect(server.Start(pair.b, pair.b, {}) && client.Start(pair.a, pair.a, {}), "start");
  Result result;
  const std::uint64_t id =
      client.Request("stream", util::JsonValue(nullptr), remote::Lane::Bulk, result.Handler());
  Expect(WaitUntil([&]() { return held.load() == id; }), "the handler holds the request");
  Expect(!server.IsCancelled(id), "not cancelled yet");
  Expect(client.Cancel(id), "cancel is sent");
  Expect(WaitUntil([&]() { return result.done.load(); }), "the terminal reply arrives");
  Expect(cancelled.load() == id, "the server's cancel hook saw the id");
  std::lock_guard lock(result.mutex);
  Expect(result.error.has_value() && result.error->code == remote::kErrorCancelled,
         "the caller sees the cancellation");
}

// A pending request is answered (with an error) when the connection goes away,
// and the close is reported once.
void TestPendingRequestsAreAnsweredOnClose() {
  Pair pair;
  remote::RemotePeer client;
  std::atomic<int> closes{0};
  client.OnClosed([&](std::string_view) { closes.fetch_add(1); });
  Expect(client.Start(pair.a, pair.a, {}), "start");
  Result result;
  client.Request("never", util::JsonValue(nullptr), remote::Lane::Interactive, result.Handler());
  ::close(pair.b);
  Expect(WaitUntil([&]() { return result.done.load() && closes.load() == 1; }),
         "the request is answered and the close reported");
  Expect(client.Request("later", util::JsonValue(nullptr), remote::Lane::Interactive,
                        result.Handler()) == 0,
         "nothing can be sent afterwards");
}

// Pings flow and are answered automatically; a peer that stops answering is
// declared dead by the heartbeat, not by waiting on the transport.
void TestHeartbeatMeasuresRttAndDetectsADeadPeer() {
  Pair pair;
  remote::RemotePeer server;
  remote::RemotePeer client;
  remote::RemotePeer::Options options;
  options.send_pings = true;
  options.heartbeat.interval_ms = 20;
  Expect(server.Start(pair.b, pair.b, {}) && client.Start(pair.a, pair.a, options), "start");
  Expect(WaitUntil([&]() { return client.rtt_ms().has_value(); }), "an RTT is measured");
  server.Stop();  // the descriptor stays open: the link is silent, not closed

  Pair silent;
  remote::RemotePeer lonely;
  std::atomic<int> closes{0};
  std::string reason;
  std::mutex reason_mutex;
  lonely.OnClosed([&](std::string_view why) {
    std::lock_guard lock(reason_mutex);
    reason = std::string(why);
    closes.fetch_add(1);
  });
  Expect(lonely.Start(silent.a, silent.a, options), "start the lonely client");
  Expect(WaitUntil([&]() { return closes.load() == 1; }, std::chrono::seconds(5)),
         "a peer that never answers is declared dead");
  std::lock_guard lock(reason_mutex);
  Expect(reason.find("link dead") != std::string::npos, "and the reason says so: " + reason);
  ::close(silent.b);
}

#endif

}  // namespace

void RegisterRemotePeerTests(std::vector<TestCase>& tests) {
  AddTest(tests, "RemotePeer/HeartbeatDeclaresDeathAfterThreeMisses",
          TestHeartbeatDeclaresDeathAfterThreeMisses);
  AddTest(tests, "RemotePeer/HeartbeatAnswerResetsMissesAndMeasuresRtt",
          TestHeartbeatAnswerResetsMissesAndMeasuresRtt);
  AddTest(tests, "RemotePeer/HelloRoundTripsAndValidates", TestHelloRoundTripsAndValidates);
  AddTest(tests, "RemotePeer/CompatibilityNamesBothReleases", TestCompatibilityNamesBothReleases);
#if defined(__unix__) || defined(__APPLE__)
  AddTest(tests, "RemotePeer/RequestsAndErrorsRoundTrip", TestRequestsAndErrorsRoundTrip);
  AddTest(tests, "RemotePeer/MalformedBodyIsRejectedAndTheConnectionContinues",
          TestMalformedBodyIsRejectedAndTheConnectionContinues);
  AddTest(tests, "RemotePeer/CancelReachesTheHandler", TestCancelReachesTheHandler);
  AddTest(tests, "RemotePeer/PendingRequestsAreAnsweredOnClose",
          TestPendingRequestsAreAnsweredOnClose);
  AddTest(tests, "RemotePeer/HeartbeatMeasuresRttAndDetectsADeadPeer",
          TestHeartbeatMeasuresRttAndDetectsADeadPeer);
#endif
}

}  // namespace microide::tests
