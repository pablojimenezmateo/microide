#include "TestSupport.h"

#include "platform/AsyncSubprocess.h"
#include "platform/Subprocess.h"
#include "platform/UnixSocket.h"
#include "project/remote/RemoteFrame.h"
#include "project/remote/RemotePeer.h"
#include "project/remote/RemoteProtocol.h"
#include "project/remote/RemoteServerClient.h"
#include "project/remote/RemoteServerPaths.h"
#include "server/ListeningPorts.h"
#include "util/JsonValue.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <linux/inet_diag.h>
#include <linux/netlink.h>
#include <linux/sock_diag.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace microide::tests {
namespace {

namespace remote = project::remote;

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
  const auto stopped = RunServer({"stop", "--socket-dir", dir.string()});
  Expect(stopped.success(), "stop: exit " + std::to_string(stopped.exit_code) + " " +
                                stopped.stdout_text + stopped.stderr_text);
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

// A script polling `microide-server status` is an observer, not a client
// (TD-2026-10-27-407): it used to restart an on-demand server's idle countdown at
// every poll, keeping it alive for good. Polled every 100 ms against a 400 ms
// timeout, the server idles out all the same.
void TestPollingStatusDoesNotKeepAnOnDemandServerAlive() {
  ShortTempDir temp;
  const std::filesystem::path dir = temp.path() / "s";
  {
    platform::AsyncSubprocess attach;
    Expect(attach.Start({Server(), "attach", "--socket-dir", dir.string(), "--idle-timeout-ms", "400"}),
           "attach starts an on-demand server");
    Expect(WaitUntil([&]() { return ::access(remote::ServerSocketPath(dir).c_str(), F_OK) == 0; },
                     std::chrono::seconds(10)),
           "its socket appears");
    attach.CloseStdin();
    Expect(WaitUntil([&]() { return !attach.IsRunning(); }, std::chrono::seconds(10)), "attach goes");
  }
  bool gone = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!gone && std::chrono::steady_clock::now() < deadline) {
    (void)RunServer({"status", "--json", "--socket-dir", dir.string()});  // one ask, no 1 s wait
    gone = ::access(remote::ServerSocketPath(dir).c_str(), F_OK) != 0;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  Expect(gone, "polled with status every 100 ms, the on-demand server still idles out");
}

// /proc/net/tcp{,6}: this user's LISTEN sockets on a loopback or wildcard address,
// whatever the table's spacing; another user's, a connected socket and one bound to
// an outside address are not forwardable ports.
void TestListeningPortsParseProcNetTcp() {
  const std::string tcp =
      "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n"
      "   0: 0100007F:0BB8 00000000:0000 0A 00000000:00000000 00:00000000 00000000  1000        0 1 1\n"
      "   1: 00000000:1F90 00000000:0000 0A 00000000:00000000 00:00000000 00000000  1000        0 2 1\n"
      "   2: 0100007F:1F91 00000000:0000 0A 00000000:00000000 00:00000000 00000000  1001        0 3 1\n"
      "   3: 0100007F:1F92 0100007F:D431 01 00000000:00000000 00:00000000 00000000  1000        0 4 1\n"
      "   4: 0A00000A:1F93 00000000:0000 0A 00000000:00000000 00:00000000 00000000  1000        0 5 1\n"
      "   5: garbage\n";
  Expect((server::ParseListeningPorts(tcp, 1000) == std::vector<std::uint16_t>{3000, 8080}),
         "loopback and wildcard listeners of this user only");
  const std::string tcp6 =
      "  sl  local_address                         remote_address                        st ...\n"
      "   0: 00000000000000000000000001000000:1538 00000000000000000000000000000000:0000 0A "
      "00000000:00000000 00:00000000 00000000  1000        0 6 1\n"
      "   1: 00000000000000000000000000000000:2328 00000000000000000000000000000000:0000 0A "
      "00000000:00000000 00:00000000 00000000  1000        0 7 1\n"
      "   2: 0000000000000000FFFF00000100007F:2329 00000000000000000000000000000000:0000 0A "
      "00000000:00000000 00:00000000 00000000  1000        0 8 1\n"
      "   3: B80D0120000000000000000001000000:232A 00000000000000000000000000000000:0000 0A "
      "00000000:00000000 00:00000000 00000000  1000        0 9 1\n";
  Expect((server::ParseListeningPorts(tcp6, 1000) == std::vector<std::uint16_t>{5432, 9000, 9001}),
         "::1, :: and ::ffff:127.0.0.1, not a global address");
}

// One SOCK_DIAG_BY_FAMILY reply, built the way the kernel frames it.
class SockDiagReply {
 public:
  SockDiagReply& Listener(int family, const std::vector<std::uint8_t>& address, std::uint16_t port,
                          std::uint32_t uid, std::uint8_t state = TCP_LISTEN) {
    inet_diag_msg message{};
    message.idiag_family = static_cast<std::uint8_t>(family);
    message.idiag_state = state;
    message.idiag_uid = uid;
    message.id.idiag_sport = htons(port);
    std::memcpy(message.id.idiag_src, address.data(), address.size());
    Append(SOCK_DIAG_BY_FAMILY, &message, sizeof(message));
    return *this;
  }
  SockDiagReply& Done() {
    const int status = 0;
    Append(NLMSG_DONE, &status, sizeof(status));
    return *this;
  }
  std::span<const std::uint8_t> bytes() const { return bytes_; }

 private:
  void Append(std::uint16_t type, const void* payload, std::size_t size) {
    nlmsghdr header{};
    header.nlmsg_len = static_cast<std::uint32_t>(NLMSG_LENGTH(size));
    header.nlmsg_type = type;
    header.nlmsg_flags = NLM_F_MULTI;
    const std::size_t at = bytes_.size();
    bytes_.resize(at + NLMSG_SPACE(size));
    std::memcpy(bytes_.data() + at, &header, sizeof(header));
    std::memcpy(bytes_.data() + at + NLMSG_HDRLEN, payload, size);
  }
  std::vector<std::uint8_t> bytes_;
};

// The netlink source keeps exactly what the /proc one keeps (the shared address
// rule): this user's LISTEN sockets on a loopback or wildcard address.
void TestListeningPortsParseSockDiag() {
  const std::vector<std::uint8_t> v4_loopback{127, 0, 0, 1}, v4_any{0, 0, 0, 0}, v4_lan{10, 0, 0, 10};
  std::vector<std::uint8_t> v6_loopback(16, 0), v6_any(16, 0), v6_mapped(16, 0), v6_global(16, 0);
  v6_loopback[15] = 1;
  v6_mapped[10] = v6_mapped[11] = 0xFF;
  v6_mapped[12] = 127;
  v6_mapped[15] = 1;
  v6_global[0] = 0x20;
  v6_global[1] = 0x01;
  v6_global[15] = 1;
  SockDiagReply reply;
  reply.Listener(AF_INET, v4_loopback, 3000, 1000)
      .Listener(AF_INET, v4_any, 8080, 1000)
      .Listener(AF_INET, v4_loopback, 8081, 1001)                   // another user's
      .Listener(AF_INET, v4_loopback, 8082, 1000, TCP_ESTABLISHED)  // not listening
      .Listener(AF_INET, v4_lan, 8083, 1000)                        // an outside address
      .Listener(AF_INET6, v6_loopback, 5432, 1000)
      .Listener(AF_INET6, v6_any, 9000, 1000)
      .Listener(AF_INET6, v6_mapped, 9001, 1000)
      .Listener(AF_INET6, v6_global, 9002, 1000)
      .Done();
  std::vector<std::uint16_t> ports;
  Expect(server::ParseSockDiagListeners(reply.bytes(), 1000, ports), "a reply with NLMSG_DONE ends the dump");
  std::sort(ports.begin(), ports.end());
  Expect((ports == std::vector<std::uint16_t>{3000, 5432, 8080, 9000, 9001}),
         "loopback, wildcard and v4-mapped loopback listeners of this user only");

  SockDiagReply partial;
  partial.Listener(AF_INET, v4_loopback, 3000, 1000);
  ports.clear();
  Expect(!server::ParseSockDiagListeners(partial.bytes(), 1000, ports) &&
             ports == std::vector<std::uint16_t>{3000},
         "a reply without NLMSG_DONE asks for the next read, keeping what it held");

  // A length past the buffer cannot be framed: the dump ends, what came before stays.
  SockDiagReply truncated;
  truncated.Listener(AF_INET, v4_loopback, 3000, 1000).Listener(AF_INET, v4_any, 8080, 1000);
  ports.clear();
  const std::span<const std::uint8_t> whole = truncated.bytes();
  Expect(server::ParseSockDiagListeners(whole.first(whole.size() - 4), 1000, ports) &&
             ports == std::vector<std::uint16_t>{3000},
         "a truncated message ends the dump without reading past the buffer");
}

// Against the running kernel: both sources see a listener this test opens, and
// they agree on the whole list (retried: another process on the machine may open
// or close a listener between the two reads).
void TestListeningPortsSourcesAgreeOnTheLiveHost() {
  const int listener = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  Expect(listener >= 0, "a TCP socket");
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t length = sizeof(address);
  Expect(::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0 &&
             ::listen(listener, 1) == 0 &&
             ::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) == 0,
         "listening on 127.0.0.1:0");
  const std::uint16_t port = ntohs(address.sin_port);
  const auto has = [&](const std::vector<std::uint16_t>& ports) {
    return std::find(ports.begin(), ports.end(), port) != ports.end();
  };
  const std::optional<std::vector<std::uint16_t>> from_proc = server::ListeningPortsFromProc();
  Expect(from_proc.has_value() && has(*from_proc), "/proc/net/tcp lists the test's listener");
  if (const std::optional<std::vector<std::uint16_t>> from_diag = server::ListeningPortsFromSockDiag()) {
    Expect(has(*from_diag), "sock_diag lists the test's listener");
    bool agreed = false;
    for (int attempt = 0; attempt < 5 && !agreed; ++attempt) {
      agreed = server::ListeningPortsFromSockDiag() == server::ListeningPortsFromProc();
    }
    Expect(agreed, "sock_diag and /proc/net/tcp name the same ports");
  }
  Expect(has(server::ListeningPorts()), "ListeningPorts lists the test's listener");
  ::close(listener);
}

// A connection that dies before the hello carries the host's own reason. The
// attach command's stderr went wherever the editor's did, so a server refusing to
// start ("... has mode 0775 ...; a parent of the socket directory must ...")
// reached the user as "the peer closed the connection" and nothing else (found
// standing up tools/qa/local-sshd.sh, whose state directory was group-writable).
void TestAConnectionThatDiesEarlyReportsWhatTheHostSaid() {
  auto client = remote::RemoteServerClient::Create();
  std::string error;
  Expect(!client->ConnectCommand({"sh", "-c", "echo 'microide-server: the socket directory is unsafe' >&2; exit 3"},
                                 remote::HelloRequest{.release = "test", .root = "/"}, &error),
         "a command that exits before the hello does not connect");
  Expect(error.find("the socket directory is unsafe") != std::string::npos,
         "the error carries what the command printed: " + error);
  // And a command that connects keeps working: its stderr is sealed, not grown.
  TemporaryDirectory root;
  auto good = remote::RemoteServerClient::Create();
  Expect(good->ConnectCommand({Server(), "serve-stdio"},
                              remote::HelloRequest{.release = "test", .root = root.path().string()}, &error),
         "a real server still connects: " + error);
}

// ports/subscribe end to end: the answer is what listens now, and a port this
// user starts listening on afterwards is pushed (the sampler's 2 s period).
void TestPortsSubscribePushesANewListener() {
  TemporaryDirectory root;
  auto client = remote::RemoteServerClient::Create();
  std::string error;
  Expect(client->ConnectCommand({Server(), "serve-stdio"},
                                remote::HelloRequest{.release = "test", .root = root.path().string()}, &error),
         "connects: " + error);
  std::mutex mutex;
  std::vector<std::uint16_t> pushed;
  const auto now = client->SubscribePorts(
      [&](const std::vector<std::uint16_t>& ports) {
        std::lock_guard lock(mutex);
        pushed = ports;
      },
      &error);
  Expect(now.has_value(), "subscribed: " + error);
  const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t length = sizeof(address);
  Expect(fd >= 0 && ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
             ::listen(fd, 1) == 0 &&
             ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0,
         "a listener on the host's loopback");
  const std::uint16_t port = ntohs(address.sin_port);
  Expect(std::find(now->begin(), now->end(), port) == now->end(), "not listening when subscribed");
  Expect(WaitUntil(
             [&] {
               std::lock_guard lock(mutex);
               return std::find(pushed.begin(), pushed.end(), port) != pushed.end();
             },
             std::chrono::seconds(20), std::chrono::milliseconds(20)),
         "the new listener is pushed");
  ::close(fd);
}

}  // namespace

// The last reference to a client can be released inside one of its own I/O-thread
// callbacks: here a response handler holds it, and the peer destroys the handler
// on that thread once it has run. The destructor joins the I/O thread, which
// cannot join itself -- std::terminate with no exception in flight, which is what
// CI's ASan job hit in two remote kernel tests (TD-2026-10-21-373, a terminal
// channel kept alive across a dropped link by its frame callback's weak lock).
void TestAClientReleasedOnItsOwnThreadIsDestroyedElsewhere() {
  TemporaryDirectory root;
  auto client = remote::RemoteServerClient::Create();
  std::string error;
  Expect(client->ConnectCommand({Server(), "serve-stdio"},
                                remote::HelloRequest{.release = "test", .root = root.path().string()}, &error),
         "connects: " + error);
  const std::weak_ptr<remote::RemoteServerClient> weak = client;
  std::atomic<bool> answered{false};
  std::atomic<bool> released_on_io_thread{false};
  Expect(client->peer().Request(remote::method::kLinkPing, util::JsonValue(util::JsonObject{}),
                                remote::Lane::Interactive,
                                [keep = client, &answered, &released_on_io_thread](
                                    std::optional<util::JsonValue>, std::optional<remote::RemotePeer::RpcError>) {
                                  released_on_io_thread = keep->peer().OnIoThread();
                                  answered = true;
                                }) != 0,
         "the request is sent");
  client.reset();  // the handler holds the only reference now
  Expect(WaitUntil([&] { return answered.load(); }, std::chrono::seconds(10)), "the ping is answered");
  Expect(released_on_io_thread.load(), "the handler ran on the client's I/O thread");
  Expect(WaitUntil([&] { return weak.expired(); }, std::chrono::seconds(10)),
         "the client went with the handler, without terminating the process");
}

void RegisterRemoteServerTests(std::vector<TestCase>& tests) {
  AddTest(tests, "RemoteServer/AConnectionThatDiesEarlyReportsWhatTheHostSaid",
          TestAConnectionThatDiesEarlyReportsWhatTheHostSaid);
  AddTest(tests, "RemoteServer/SocketDirIsCreatedPrivateAndBadModesAreRefused",
          TestSocketDirIsCreatedPrivateAndBadModesAreRefused);
  AddTest(tests, "RemoteServer/SymlinkedComponentIsRefusedByName",
          TestSymlinkedComponentIsRefusedByName);
  AddTest(tests, "RemoteServer/SessionSurvivalReadsLogindAndLinger",
          TestSessionSurvivalReadsLogindAndLinger);
  AddTest(tests, "RemoteServer/StartedServerServesWorkspacesAndDoesNotHoldStdio",
          TestStartedServerServesWorkspacesAndDoesNotHoldStdio);
  AddTest(tests, "RemoteServer/PollingStatusDoesNotKeepAnOnDemandServerAlive",
          TestPollingStatusDoesNotKeepAnOnDemandServerAlive);
  AddTest(tests, "RemoteServer/AttachStartsOnDemandAndTheServerIdlesOut",
          TestAttachStartsOnDemandAndTheServerIdlesOut);
  AddTest(tests, "RemoteServer/ListeningPortsParseProcNetTcp", TestListeningPortsParseProcNetTcp);
  AddTest(tests, "RemoteServer/ListeningPortsParseSockDiag", TestListeningPortsParseSockDiag);
  AddTest(tests, "RemoteServer/ListeningPortsSourcesAgreeOnTheLiveHost",
          TestListeningPortsSourcesAgreeOnTheLiveHost);
  AddTest(tests, "RemoteServer/PortsSubscribePushesANewListener", TestPortsSubscribePushesANewListener);
  AddTest(tests, "RemoteServer/AClientReleasedOnItsOwnThreadIsDestroyedElsewhere",
          TestAClientReleasedOnItsOwnThreadIsDestroyedElsewhere);
}

}  // namespace microide::tests
