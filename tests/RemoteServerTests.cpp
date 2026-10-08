#include "TestSupport.h"

#include "platform/AsyncSubprocess.h"
#include "platform/Subprocess.h"
#include "platform/UnixSocket.h"
#include "project/remote/RemoteFrame.h"
#include "project/remote/RemotePeer.h"
#include "project/remote/RemoteProtocol.h"
#include "project/remote/RemoteServerPaths.h"
#include "util/JsonValue.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace microide::tests {
namespace {

namespace remote = project::remote;

#if defined(__unix__) || defined(__APPLE__)

// The AF_UNIX limit makes the usual long temp paths unusable for a socket, so the
// daemon tests use a short directory of their own.
class ShortTempDir {
 public:
  ShortTempDir() {
    std::string pattern =
        (std::filesystem::temp_directory_path() / "mis.XXXXXX").string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    Expect(::mkdtemp(buffer.data()) != nullptr, "mkdtemp");
    path_ = buffer.data();
  }
  ~ShortTempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

std::string Server() { return MICROIDE_SERVER_BINARY; }

platform::SubprocessResult RunServer(const std::vector<std::string>& args) {
  std::vector<std::string> argv = {Server()};
  argv.insert(argv.end(), args.begin(), args.end());
  return platform::RunSubprocess(argv, platform::SubprocessOptions{.timeout_ms = 20000});
}

unsigned Mode(const std::filesystem::path& path) {
  struct stat st{};
  ::lstat(path.c_str(), &st);
  return st.st_mode & 07777u;
}

// A missing directory is created 0700; an existing group-writable one is refused
// with its mode and the expected mode, and never "fixed".
void TestSocketDirIsCreatedPrivateAndBadModesAreRefused() {
  ShortTempDir temp;
  const std::filesystem::path dir = temp.path() / "a" / "server";
  std::string error;
  Expect(remote::EnsureServerSocketDir(dir, &error), "a missing directory is created: " + error);
  Expect(Mode(dir) == 0700 && Mode(dir.parent_path()) == 0700, "created mode 0700");

  const std::filesystem::path loose = temp.path() / "loose";
  std::filesystem::create_directory(loose);
  ::chmod(loose.c_str(), 0770);
  Expect(!remote::EnsureServerSocketDir(loose, &error), "a 0770 directory is refused");
  Expect(error.find("0770") != std::string::npos && error.find("0700") != std::string::npos &&
             error.find(loose.string()) != std::string::npos,
         "the refusal names the directory, its mode and the expected one: " + error);
  Expect(Mode(loose) == 0770, "and leaves it as it was");
}

void TestSymlinkedComponentIsRefusedByName() {
  ShortTempDir temp;
  std::filesystem::create_directory(temp.path() / "real");
  std::filesystem::create_directory_symlink(temp.path() / "real", temp.path() / "link");
  std::string error;
  Expect(!remote::EnsureServerSocketDir(temp.path() / "link" / "server", &error),
         "a path through a symlink is refused");
  Expect(error.find((temp.path() / "link").string()) != std::string::npos &&
             error.find("symlink") != std::string::npos,
         "naming the symlinked component: " + error);
  Expect(!remote::EnsureServerSocketDir(temp.path() / std::string(120, 'x'), &error) &&
             error.find("AF_UNIX") != std::string::npos,
         "a socket path over the AF_UNIX limit is refused up front");
}

void TestSessionSurvivalReadsLogindAndLinger() {
  TemporaryDirectory root;
  WriteFile(root.path() / "etc/systemd/logind.conf", "[Login]\n#KillUserProcesses=yes\nKillUserProcesses=no\n");
  Expect(!remote::ReadSessionSurvival(root.path(), "alice").kill_user_processes, "main file: no");
  WriteFile(root.path() / "etc/systemd/logind.conf.d/50-kill.conf", "[Login]\nKillUserProcesses = yes\n");
  WriteFile(root.path() / "etc/systemd/logind.conf.d/10-early.conf", "[Login]\nKillUserProcesses=no\n");
  Expect(remote::ReadSessionSurvival(root.path(), "alice").kill_user_processes,
         "the last drop-in in lexical order wins");
  WriteFile(root.path() / "etc/systemd/logind.conf.d/90-other.conf", "[Other]\nKillUserProcesses=no\n");
  Expect(remote::ReadSessionSurvival(root.path(), "alice").kill_user_processes,
         "a key outside [Login] does not count");
  Expect(!remote::ReadSessionSurvival(root.path(), "alice").linger, "no linger file");
  WriteFile(root.path() / "var/lib/systemd/linger/alice", "");
  Expect(remote::ReadSessionSurvival(root.path(), "alice").linger, "linger file present");
  Expect(!remote::ReadSessionSurvival(root.path(), "../alice").linger, "a user name is not a path");
}

// Talk to a running server over its socket.
struct Client {
  remote::RemotePeer peer;
  bool Connect(const std::filesystem::path& socket_dir) {
    const int fd = platform::ConnectUnixSocket(remote::ServerSocketPath(socket_dir));
    return fd >= 0 && peer.Start(fd, fd, {});
  }
  std::optional<util::JsonValue> Ask(std::string_view method, const util::JsonValue& params,
                                     std::optional<remote::RemotePeer::RpcError>* error = nullptr) {
    std::mutex mutex;
    std::optional<util::JsonValue> answer;
    std::optional<remote::RemotePeer::RpcError> rpc_error;
    std::atomic<bool> done{false};
    peer.Request(method, params, remote::Lane::Interactive,
                 [&](std::optional<util::JsonValue> result,
                     std::optional<remote::RemotePeer::RpcError> e) {
                   std::lock_guard lock(mutex);
                   answer = std::move(result);
                   rpc_error = std::move(e);
                   done = true;
                 });
    Expect(WaitUntil([&]() { return done.load(); }, std::chrono::seconds(10)), "answered");
    std::lock_guard lock(mutex);
    if (error != nullptr) {
      *error = rpc_error;
    }
    return answer;
  }
};

util::JsonValue Hello(std::string_view root, std::int64_t protocol = remote::kProtocolVersion,
                      std::int64_t min_protocol = remote::kMinProtocolVersion) {
  return remote::ToJson(remote::HelloRequest{
      .protocol = protocol, .min_protocol = min_protocol, .release = "test", .root = std::string(root)});
}

// `start` daemonizes and returns — and the pipe it was started with reaches EOF,
// because the daemon does not hold the starter's stdio (or `ssh host -- … start`
// would never return). Hello, status listing two workspaces, a refused handshake,
// and a hand-started server that does not idle out.
void TestStartedServerServesWorkspacesAndDoesNotHoldStdio() {
  ShortTempDir temp;
  const std::filesystem::path dir = temp.path() / "s";
  const auto started = RunServer({"start", "--socket-dir", dir.string(), "--idle-timeout-ms", "50"});
  Expect(started.success() && !started.timed_out,
         "start returns, with its stdout at EOF: " + started.stdout_text + started.stderr_text);
  Expect(started.stdout_text.find("started") != std::string::npos, "and says where");

  Client a;
  Client b;
  Expect(a.Connect(dir) && b.Connect(dir), "two clients connect");
  std::optional<remote::RemotePeer::RpcError> error;
  const auto reply = a.Ask(remote::method::kServerHello, Hello("/srv/one"), &error);
  std::string why;
  const auto hello = reply.has_value() ? remote::HelloReplyFromJson(*reply, &why) : std::nullopt;
  Expect(hello.has_value() && hello->daemon_epoch.size() == 32, "a valid hello reply: " + why);
  Expect(b.Ask(remote::method::kServerHello, Hello("/srv/two")).has_value(), "second hello");

  const auto status = RunServer({"status", "--socket-dir", dir.string()});
  Expect(status.success() && status.stdout_text.find("/srv/one") != std::string::npos &&
             status.stdout_text.find("/srv/two") != std::string::npos,
         "status lists both workspaces: " + status.stdout_text);

  Client old;
  Expect(old.Connect(dir), "an old client connects");
  (void)old.Ask(remote::method::kServerHello, Hello("", 99, 99), &error);
  Expect(error.has_value() && error->code == remote::kErrorIncompatible &&
             error->message.find("test") != std::string::npos,
         "an incompatible hello is refused naming both releases");

  // A hand-started server outlives its idle timeout with nobody attached.
  a.peer.Stop();
  b.peer.Stop();
  old.peer.Stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  Expect(::access(remote::ServerSocketPath(dir).c_str(), F_OK) == 0,
         "a hand-started server never idles out");
  Expect(RunServer({"stop", "--socket-dir", dir.string()}).success(), "stop");
  Expect(WaitUntil([&]() { return ::access(remote::ServerSocketPath(dir).c_str(), F_OK) != 0; },
                   std::chrono::seconds(10)),
         "stop removes the socket");
}

// `attach` starts a server on demand when none is running and relays the protocol
// over its stdio; once the client goes and the idle timeout passes, the on-demand
// server exits and removes its socket.
void TestAttachStartsOnDemandAndTheServerIdlesOut() {
  ShortTempDir temp;
  const std::filesystem::path dir = temp.path() / "s";
  platform::AsyncSubprocess attach;
  Expect(attach.Start({Server(), "attach", "--socket-dir", dir.string(), "--idle-timeout-ms", "100"}),
         "attach starts");
  std::string request;
  remote::AppendFrame(request, remote::FrameType::Request, remote::Lane::Interactive, 1,
                      util::SerializeJson(util::JsonValue(util::JsonObject{
                          {"method", util::JsonValue(std::string(remote::method::kServerHello))},
                          {"params", Hello("/srv/attached")}})));
  Expect(attach.Write(request), "the hello goes out over attach's stdin");
  remote::FrameDecoder decoder;
  std::optional<remote::Frame> reply;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (!reply.has_value() && std::chrono::steady_clock::now() < deadline) {
    const auto bytes = attach.Read(65536, 200);
    if (!bytes.has_value()) {
      break;
    }
    decoder.Feed(*bytes);
    reply = decoder.Next();
  }
  Expect(reply.has_value() && reply->type == remote::FrameType::Response && reply->id == 1,
         "the reply comes back through the relay");
  Expect(util::ParseJson(reply->payload)->HasKey("result"), "and it is a result");
  Expect(RunServer({"status", "--socket-dir", dir.string()}).stdout_text.find("/srv/attached") !=
             std::string::npos,
         "the on-demand server lists the attached workspace");

  attach.CloseStdin();
  Expect(WaitUntil([&]() { return !attach.IsRunning(); }, std::chrono::seconds(10)),
         "attach exits when its client is done");
  Expect(WaitUntil([&]() { return ::access(remote::ServerSocketPath(dir).c_str(), F_OK) != 0; },
                   std::chrono::seconds(10)),
         "the on-demand server idles out and removes its socket");
}

#endif

}  // namespace

void RegisterRemoteServerTests(std::vector<TestCase>& tests) {
#if defined(__unix__) || defined(__APPLE__)
  AddTest(tests, "RemoteServer/SocketDirIsCreatedPrivateAndBadModesAreRefused",
          TestSocketDirIsCreatedPrivateAndBadModesAreRefused);
  AddTest(tests, "RemoteServer/SymlinkedComponentIsRefusedByName",
          TestSymlinkedComponentIsRefusedByName);
  AddTest(tests, "RemoteServer/SessionSurvivalReadsLogindAndLinger",
          TestSessionSurvivalReadsLogindAndLinger);
  AddTest(tests, "RemoteServer/StartedServerServesWorkspacesAndDoesNotHoldStdio",
          TestStartedServerServesWorkspacesAndDoesNotHoldStdio);
  AddTest(tests, "RemoteServer/AttachStartsOnDemandAndTheServerIdlesOut",
          TestAttachStartsOnDemandAndTheServerIdlesOut);
#else
  (void)tests;
#endif
}

}  // namespace microide::tests
