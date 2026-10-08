#include "TestSupport.h"
#include "RemoteServerTestSupport.h"

#include "platform/Subprocess.h"
#include "platform/UnixSocket.h"
#include "project/remote/RemoteFrame.h"
#include "project/remote/RemotePeer.h"
#include "project/remote/RemoteServerPaths.h"
#include "util/JsonValue.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <csignal>
#include <unistd.h>
#endif

namespace microide::tests {
namespace {

namespace remote = project::remote;

#if defined(__unix__) || defined(__APPLE__)

using ShortDir = ShortServerDir;

// A client that collects each handle's output and exit, acknowledging as it goes.
class ProcClient {
 public:
  struct Output {
    std::string out;
    std::string err;
    std::optional<std::int64_t> code;
    std::optional<std::int64_t> signal;
  };

  explicit ProcClient(const std::filesystem::path& dir) {
    peer_.OnContent([this](remote::FrameType type, std::uint64_t handle, std::string bytes) {
      std::lock_guard lock(mutex_);
      Output& output = outputs_[handle];
      (type == remote::FrameType::ProcStderr ? output.err : output.out) += bytes;
      util::JsonObject ack;
      ack["handle"] = util::JsonValue(static_cast<std::int64_t>(handle));
      ack["stdout"] = util::JsonValue(static_cast<std::int64_t>(base_out_[handle] + output.out.size()));
      ack["stderr"] = util::JsonValue(static_cast<std::int64_t>(base_err_[handle] + output.err.size()));
      peer_.Notify("proc/ack", util::JsonValue(std::move(ack)));
    });
    peer_.OnNotification("proc/exit", [this](std::uint64_t handle, const util::JsonValue& params) {
      std::lock_guard lock(mutex_);
      Output& output = outputs_[handle];
      if (params.HasKey("signal")) {
        output.signal = params["signal"].AsInt();
      } else {
        output.code = params["code"].AsInt();
      }
    });
    const int fd = platform::ConnectUnixSocket(remote::ServerSocketPath(dir));
    Expect(fd >= 0 && peer_.Start(fd, fd, {}), "the client connects");
  }

  std::optional<util::JsonValue> Call(std::string_view method, util::JsonValue params,
                                      std::string* error = nullptr) {
    std::mutex mutex;
    std::optional<util::JsonValue> answer;
    std::string why;
    std::atomic<bool> done{false};
    peer_.Request(method, params, remote::Lane::Interactive,
                  [&](std::optional<util::JsonValue> result,
                      std::optional<remote::RemotePeer::RpcError> rpc_error) {
                    std::lock_guard lock(mutex);
                    answer = std::move(result);
                    if (rpc_error.has_value()) why = rpc_error->message;
                    done = true;
                  });
    Expect(WaitUntil([&]() { return done.load(); }, std::chrono::seconds(10)), "answered");
    std::lock_guard lock(mutex);
    if (error != nullptr) *error = why;
    return answer;
  }

  std::uint64_t Spawn(const std::vector<std::string>& argv, bool keep = false, int* pid = nullptr) {
    util::JsonArray args;
    for (const std::string& arg : argv) args.push_back(util::JsonValue(arg));
    util::JsonObject params;
    params["argv"] = util::JsonValue(std::move(args));
    params["cwd"] = util::JsonValue("/");
    params["keep_on_detach"] = util::JsonValue(keep);
    std::string error;
    const auto result = Call("proc/spawn", util::JsonValue(std::move(params)), &error);
    Expect(result.has_value() && (*result)["handle"].AsInt() > 0, "spawned: " + error);
    if (pid != nullptr) *pid = static_cast<int>((*result)["pid"].AsInt());
    return static_cast<std::uint64_t>((*result)["handle"].AsInt());
  }

  // Resume `handle` from `out`/`err` (the offsets this client says it has through).
  void Attach(std::uint64_t handle, std::uint64_t out, std::uint64_t err) {
    {
      std::lock_guard lock(mutex_);
      base_out_[handle] = out;
      base_err_[handle] = err;
    }
    util::JsonObject params;
    params["handle"] = util::JsonValue(static_cast<std::int64_t>(handle));
    params["stdout"] = util::JsonValue(static_cast<std::int64_t>(out));
    params["stderr"] = util::JsonValue(static_cast<std::int64_t>(err));
    std::string error;
    Expect(Call("proc/attach", util::JsonValue(std::move(params)), &error).has_value(),
           "attached: " + error);
  }

  void Notify(std::string_view method, std::uint64_t handle, std::string_view signal = {}) {
    util::JsonObject params;
    params["handle"] = util::JsonValue(static_cast<std::int64_t>(handle));
    if (!signal.empty()) params["signal"] = util::JsonValue(std::string(signal));
    peer_.Notify(method, util::JsonValue(std::move(params)));
  }

  Output Wait(std::uint64_t handle) {
    Expect(WaitUntil(
               [&]() {
                 std::lock_guard lock(mutex_);
                 const auto it = outputs_.find(handle);
                 return it != outputs_.end() && (it->second.code || it->second.signal);
               },
               std::chrono::seconds(20)),
           "the process exits");
    std::lock_guard lock(mutex_);
    return outputs_[handle];
  }

  std::string StdoutSoFar(std::uint64_t handle) {
    std::lock_guard lock(mutex_);
    return outputs_[handle].out;
  }

  void Drop() { peer_.Stop(); }

 private:
  remote::RemotePeer peer_;
  std::mutex mutex_;
  std::map<std::uint64_t, Output> outputs_;
  std::map<std::uint64_t, std::uint64_t> base_out_;
  std::map<std::uint64_t, std::uint64_t> base_err_;
};

// argv is an ARRAY end to end: an argument with a space, both quotes, `$HOME` and
// a newline reaches the process byte for byte — no shell, nothing joined.
void TestArgvArrivesIntact() {
  ShortDir dir;
  StartServer(dir);
  ProcClient client(dir.path());
  const std::string tricky = "a b 'single' \"double\" $HOME `x` \\back\nsecond line";
  const auto output = client.Wait(client.Spawn({"printf", "%s", tricky}));
  Expect(output.out == tricky, "stdout is byte-identical to the argument: [" + output.out + "]");
  Expect(output.code == 0, "exit 0");
}

void TestExitCodesStderrAndSignals() {
  ShortDir dir;
  StartServer(dir);
  ProcClient client(dir.path());
  const auto failed = client.Wait(client.Spawn({"sh", "-c", "echo oops >&2; exit 7"}));
  Expect(failed.code == 7 && failed.err == "oops\n" && failed.out.empty(),
         "exit status and stderr arrive separately");
  const std::uint64_t sleeper = client.Spawn({"sleep", "30"});
  client.Notify("proc/signal", sleeper, "TERM");
  const auto signalled = client.Wait(sleeper);
  Expect(signalled.signal == SIGTERM, "a signalled process reports its signal");
  const auto missing = client.Wait(client.Spawn({"no-such-binary-anywhere-xyz"}));
  Expect(missing.code == 127, "a missing program exits 127, like a shell");
}

// A kept process keeps running through a dropped connection, and the reattaching
// client resumes its stdout from the offset it had: nothing duplicated, nothing lost.
void TestKeptProcessResumesFromItsOffset() {
  ShortDir dir;
  StartServer(dir);
  std::string expected;
  for (int i = 1; i <= 60; ++i) expected += "line" + std::to_string(i) + "\n";
  std::uint64_t handle = 0;
  std::string first_half;
  {
    ProcClient first(dir.path());
    handle = first.Spawn({"sh", "-c", "for i in $(seq 1 60); do echo line$i; sleep 0.01; done"},
                         /*keep=*/true);
    Expect(WaitUntil([&]() { return first.StdoutSoFar(handle).size() >= 30; },
                     std::chrono::seconds(10)),
           "some output arrives before the drop");
    first_half = first.StdoutSoFar(handle);
    first.Drop();
  }
  ProcClient second(dir.path());
  second.Attach(handle, first_half.size(), 0);
  const auto rest = second.Wait(handle);
  Expect(first_half + rest.out == expected,
         "the two halves are exactly the output, no duplicate and no gap");
  Expect(rest.code == 0, "and the exit arrives");
  second.Notify("proc/release", handle);
}

// A process NOT kept on detach is terminated when its client goes.
void TestUnkeptProcessIsTerminatedOnDetach() {
  ShortDir dir;
  StartServer(dir);
  int pid = -1;
  {
    ProcClient client(dir.path());
    (void)client.Spawn({"sleep", "60"}, /*keep=*/false, &pid);
    Expect(pid > 0 && ::kill(pid, 0) == 0, "it runs");
    client.Drop();
  }
  Expect(WaitUntil([&]() { return ::kill(pid, 0) != 0; }, std::chrono::seconds(15)),
         "the language-server case: it does not outlive its client");
}

// A kept process that exits while nobody is attached delivers its output and exit
// on the next attach.
void TestExitWhileDetachedIsDeliveredOnAttach() {
  ShortDir dir;
  StartServer(dir);
  std::uint64_t handle = 0;
  {
    ProcClient client(dir.path());
    handle = client.Spawn({"sh", "-c", "sleep 0.3; echo bye; exit 3"}, /*keep=*/true);
    client.Drop();
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(800));
  ProcClient later(dir.path());
  later.Attach(handle, 0, 0);
  const auto output = later.Wait(handle);
  Expect(output.out == "bye\n" && output.code == 3, "output and exit are delivered on attach");
  std::string error;
  later.Notify("proc/release", handle);
  util::JsonObject params;
  params["handle"] = util::JsonValue(static_cast<std::int64_t>(handle));
  params["stdout"] = util::JsonValue(std::int64_t{0});
  params["stderr"] = util::JsonValue(std::int64_t{0});
  Expect(WaitUntil([&]() {
           return !later.Call("proc/attach", util::JsonValue(params), &error).has_value();
         }),
         "a released handle is forgotten");
}

void TestMalformedSpawnIsRefused() {
  ShortDir dir;
  StartServer(dir);
  ProcClient client(dir.path());
  std::string error;
  Expect(!client.Call("proc/spawn", *util::ParseJson(R"({"argv":[]})"), &error).has_value() &&
             !error.empty(),
         "an empty argv is refused");
  Expect(!client.Call("proc/spawn", *util::ParseJson(R"({"argv":["ls"],"cwd":"relative"})"), &error)
              .has_value(),
         "a relative cwd is refused");
  Expect(!client.Call("proc/spawn", *util::ParseJson(R"({"argv":["ls"],"env":{"1BAD":"x"}})"), &error)
              .has_value(),
         "a bad env name is refused");
}

#endif

}  // namespace

void RegisterRemoteProcessTests(std::vector<TestCase>& tests) {
#if defined(__unix__) || defined(__APPLE__)
  AddTest(tests, "RemoteProcess/ArgvArrivesIntact", TestArgvArrivesIntact);
  AddTest(tests, "RemoteProcess/ExitCodesStderrAndSignals", TestExitCodesStderrAndSignals);
  AddTest(tests, "RemoteProcess/KeptProcessResumesFromItsOffset", TestKeptProcessResumesFromItsOffset);
  AddTest(tests, "RemoteProcess/UnkeptProcessIsTerminatedOnDetach",
          TestUnkeptProcessIsTerminatedOnDetach);
  AddTest(tests, "RemoteProcess/ExitWhileDetachedIsDeliveredOnAttach",
          TestExitWhileDetachedIsDeliveredOnAttach);
  AddTest(tests, "RemoteProcess/MalformedSpawnIsRefused", TestMalformedSpawnIsRefused);
#else
  (void)tests;
#endif
}

}  // namespace microide::tests
