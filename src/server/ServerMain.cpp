// microide-server: the host half of a remote project (dev-docs/design/remote-projects.md
// § 6.6). One daemon per user; subcommands:
//
//   start   [--on-demand] [--socket-dir D] [--idle-timeout-ms N]   daemonize and serve
//   stop    [--socket-dir D]                                       ask it to exit
//   status  [--socket-dir D]                                       print what it serves
//   attach  [--socket-dir D] [--idle-timeout-ms N]                 relay stdio to it,
//                                                                  starting one on demand
//   serve-stdio                                                    serve ONE connection on
//                                                                  stdin/stdout (tests)
//
// Links microide_kernel only: no SDL, no shell, no fonts (CheckServerStaysFreeOf…).
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "platform/UnixSocket.h"
#include "project/remote/RemotePeer.h"
#include "project/remote/RemoteProtocol.h"
#include "project/remote/RemoteServerPaths.h"
#include "server/RemoteServer.h"
#include "util/JsonFormat.h"
#include "util/JsonValue.h"
#include "util/Log.h"
#include "util/Parse.h"

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifndef MICROIDE_VERSION
#define MICROIDE_VERSION "dev"
#endif

namespace {

namespace remote = microide::project::remote;

struct Options {
  std::string command;
  std::filesystem::path socket_dir;
  bool on_demand = false;
  std::chrono::milliseconds idle_timeout{10 * 60 * 1000};
};

int Usage() {
  std::cerr << "usage: microide-server <start|stop|status|attach|serve-stdio> "
               "[--socket-dir DIR] [--on-demand] [--idle-timeout-ms N]\n";
  return 2;
}

// The user is $HOME and getuid(), never NSS: a static musl build has no NSS, and the
// server must not depend on what a directory service says about the user.
std::filesystem::path HomeDirectory() {
  if (const char* home = std::getenv("HOME"); home != nullptr && *home == '/') {
    return home;
  }
  return {};
}

std::string UserName() {
  if (const char* user = std::getenv("USER"); user != nullptr) {
    return user;
  }
  return {};
}

std::optional<Options> ParseOptions(int argc, char** argv) {
  if (argc < 2) {
    return std::nullopt;
  }
  Options options;
  options.command = argv[1];
  for (int i = 2; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--socket-dir" && i + 1 < argc) {
      options.socket_dir = argv[++i];
    } else if (arg == "--on-demand") {
      options.on_demand = true;
    } else if (arg == "--idle-timeout-ms" && i + 1 < argc) {
      const auto ms = microide::util::ParseInt64(argv[++i]);
      if (!ms.has_value() || *ms < 0) {
        return std::nullopt;
      }
      options.idle_timeout = std::chrono::milliseconds(*ms);
    } else {
      return std::nullopt;
    }
  }
  if (options.socket_dir.empty()) {
    if (const char* override_dir = std::getenv("MICROIDE_SERVER_SOCKET_DIR");
        override_dir != nullptr && *override_dir != '\0') {
      options.socket_dir = override_dir;
    } else if (const auto home = HomeDirectory(); !home.empty()) {
      options.socket_dir = remote::DefaultServerSocketDir(home);
    }
  }
  return options;
}

#if defined(__unix__) || defined(__APPLE__)

// Diagnostics go to a size-capped file beside the socket: a daemon's stderr is
// /dev/null, and an unbounded log on a user's home is its own incident.
void InstallLogFile(const std::filesystem::path& path) {
  constexpr off_t kMaxLogBytes = 1024 * 1024;
  microide::util::SetLogSink([path](std::string_view message) {
    struct stat st{};
    if (::stat(path.c_str(), &st) == 0 && st.st_size > kMaxLogBytes) {
      std::filesystem::path old = path;
      old += ".1";
      ::rename(path.c_str(), old.c_str());
    }
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0) {
      return;
    }
    std::string line(message);
    line.push_back('\n');
    const ssize_t written = ::write(fd, line.data(), line.size());
    (void)written;
    ::close(fd);
  });
}

bool Connectable(const std::filesystem::path& socket) {
  const int fd = microide::platform::ConnectUnixSocket(socket);
  if (fd < 0) {
    return false;
  }
  ::close(fd);
  return true;
}

int Start(const Options& options) {
  std::string error;
  if (!remote::EnsureServerSocketDir(options.socket_dir, &error)) {
    std::cerr << "microide-server: " << error << '\n';
    return 1;
  }
  const std::filesystem::path socket = remote::ServerSocketPath(options.socket_dir);
  if (Connectable(socket)) {
    std::cout << "microide-server already running at " << socket.string() << '\n';
    return 0;
  }
  const pid_t child = ::fork();
  if (child < 0) {
    std::cerr << "microide-server: fork failed: " << std::strerror(errno) << '\n';
    return 1;
  }
  if (child == 0) {
    // Leave the starter's session and process group, then fork again so the daemon
    // is not a session leader (it can never reacquire a controlling terminal).
    ::setsid();
    if (::fork() != 0) {
      ::_exit(0);
    }
    // Detach stdio BEFORE anything else happens: `ssh host -- microide-server
    // attach` must not be held open by a daemon that inherited its descriptors.
    const int null_fd = ::open("/dev/null", O_RDWR | O_CLOEXEC);
    if (null_fd >= 0) {
      ::dup2(null_fd, 0);
      ::dup2(null_fd, 1);
      ::dup2(null_fd, 2);
      ::close(null_fd);
    }
    ::umask(077);
    if (::chdir("/") != 0) {
      ::_exit(1);
    }
    ::signal(SIGPIPE, SIG_IGN);
    InstallLogFile(remote::ServerLogPath(options.socket_dir));
    std::string listen_error;
    const int listen_fd = microide::platform::ListenUnixSocket(socket, &listen_error);
    if (listen_fd < 0) {
      microide::util::Log("cannot listen: " + listen_error);
      ::_exit(1);
    }
    int code = 0;
    {
      // Scoped, so its destructor runs before _exit: that is what joins the
      // connections' threads and ends the processes and terminal shells it owns.
      // _exit straight out of RunListener left them running on the host for nobody.
      microide::server::RemoteServer server(microide::server::RemoteServer::Config{
          .socket_path = socket,
          .release = MICROIDE_VERSION,
          .on_demand = options.on_demand,
          .idle_timeout = options.idle_timeout,
          .session_survival = remote::ReadSessionSurvival("/", UserName()),
      });
      code = server.RunListener(listen_fd);
    }
    ::_exit(code);
  }
  int status = 0;
  ::waitpid(child, &status, 0);  // the intermediate child, which exits at once
  // Success is the socket answering — never the spawned process's exit.
  for (int attempt = 0; attempt < 500; ++attempt) {
    if (Connectable(socket)) {
      std::cout << "microide-server started at " << socket.string() << '\n';
      return 0;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  std::cerr << "microide-server: the server did not come up; see "
            << remote::ServerLogPath(options.socket_dir).string() << '\n';
  return 1;
}

// One request to the running server, synchronously; for status and stop.
std::optional<microide::util::JsonValue> Ask(const Options& options, std::string_view method,
                                             std::string* error) {
  const std::filesystem::path socket = remote::ServerSocketPath(options.socket_dir);
  const int fd = microide::platform::ConnectUnixSocket(socket);
  if (fd < 0) {
    *error = "no microide-server is running at " + socket.string();
    return std::nullopt;
  }
  remote::RemotePeer peer;
  if (!peer.Start(fd, fd, {})) {
    *error = "could not talk to the server";
    return std::nullopt;
  }
  std::mutex mutex;
  std::condition_variable done;
  std::optional<microide::util::JsonValue> answer;
  bool finished = false;
  peer.Request(method, microide::util::JsonValue(nullptr), remote::Lane::Interactive,
               [&](std::optional<microide::util::JsonValue> result,
                   std::optional<remote::RemotePeer::RpcError> rpc_error) {
                 std::lock_guard lock(mutex);
                 if (rpc_error.has_value()) {
                   *error = rpc_error->message;
                 }
                 answer = std::move(result);
                 finished = true;
                 done.notify_all();
               });
  std::unique_lock lock(mutex);
  if (!done.wait_for(lock, std::chrono::seconds(10), [&]() { return finished; })) {
    *error = "the server did not answer";
  }
  lock.unlock();
  peer.Stop();
  return answer;
}

int Status(const Options& options) {
  std::string error;
  const auto status = Ask(options, "server/status", &error);
  if (!status.has_value()) {
    std::cerr << "microide-server: " << error << '\n';
    return 1;
  }
  const auto formatted = microide::util::FormatJson(microide::util::SerializeJson(*status), "  ");
  std::cout << (formatted.ok ? formatted.text : microide::util::SerializeJson(*status)) << '\n';
  return 0;
}

int Stop(const Options& options) {
  std::string error;
  if (!Ask(options, remote::method::kServerShutdown, &error).has_value()) {
    std::cerr << "microide-server: " << error << '\n';
    return 1;
  }
  std::cout << "microide-server stopped\n";
  return 0;
}

// Copy stdin to the socket and the socket to stdout until either side ends.
int Relay(int socket_fd) {
  char buffer[64 * 1024];
  bool stdin_open = true;
  for (;;) {
    pollfd fds[2] = {{socket_fd, POLLIN, 0}, {stdin_open ? 0 : -1, POLLIN, 0}};
    if (::poll(fds, 2, -1) < 0) {
      if (errno == EINTR) {
        continue;
      }
      return 1;
    }
    if ((fds[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
      const ssize_t n = ::read(socket_fd, buffer, sizeof(buffer));
      if (n <= 0) {
        return 0;  // the server closed: so does the relay
      }
      for (ssize_t off = 0; off < n;) {
        const ssize_t w = ::write(1, buffer + off, static_cast<std::size_t>(n - off));
        if (w <= 0) {
          if (w < 0 && errno == EINTR) continue;
          return 1;
        }
        off += w;
      }
    }
    if (stdin_open && (fds[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
      const ssize_t n = ::read(0, buffer, sizeof(buffer));
      if (n <= 0) {
        stdin_open = false;
        ::shutdown(socket_fd, SHUT_WR);  // the client is done talking; let replies drain
        continue;
      }
      for (ssize_t off = 0; off < n;) {
        const ssize_t w = ::send(socket_fd, buffer + off, static_cast<std::size_t>(n - off),
                                 MSG_NOSIGNAL);
        if (w <= 0) {
          if (w < 0 && errno == EINTR) continue;
          return 1;
        }
        off += w;
      }
    }
  }
}

int Attach(const Options& options, const char* self) {
  const std::filesystem::path socket = remote::ServerSocketPath(options.socket_dir);
  int fd = microide::platform::ConnectUnixSocket(socket);
  if (fd < 0) {
    // Start one on demand, through this same binary, and wait for it like `start`.
    const pid_t child = ::fork();
    if (child == 0) {
      const int null_fd = ::open("/dev/null", O_RDWR | O_CLOEXEC);
      if (null_fd >= 0) {
        ::dup2(null_fd, 1);  // `start` prints; our stdout is the protocol
        ::close(null_fd);
      }
      const std::string idle = std::to_string(options.idle_timeout.count());
      const std::string dir = options.socket_dir.string();
      ::execl(self, self, "start", "--on-demand", "--socket-dir", dir.c_str(), "--idle-timeout-ms",
              idle.c_str(), static_cast<char*>(nullptr));
      ::_exit(127);
    }
    int status = 0;
    if (child < 0 || ::waitpid(child, &status, 0) < 0 || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
      std::cerr << "microide-server: could not start the server\n";
      return 1;
    }
    fd = microide::platform::ConnectUnixSocket(socket);
    if (fd < 0) {
      std::cerr << "microide-server: started, but cannot connect to " << socket.string() << '\n';
      return 1;
    }
  }
  const int result = Relay(fd);
  ::close(fd);
  return result;
}

int ServeStdio() {
  microide::util::SetLogSink([](std::string_view) {});  // stdout is the protocol
  microide::server::RemoteServer server(microide::server::RemoteServer::Config{
      .release = MICROIDE_VERSION,
      .session_survival = remote::ReadSessionSurvival("/", UserName()),
  });
  return server.ServeOne(0, 1);
}

#endif

}  // namespace

int main(int argc, char** argv) {
#if defined(__unix__) || defined(__APPLE__)
  ::signal(SIGPIPE, SIG_IGN);
  const std::optional<Options> options = ParseOptions(argc, argv);
  if (!options.has_value()) {
    return Usage();
  }
  if (options->command == "serve-stdio") {
    return ServeStdio();
  }
  if (options->socket_dir.empty()) {
    std::cerr << "microide-server: no socket directory ($HOME unset; pass --socket-dir)\n";
    return 1;
  }
  if (options->command == "start") {
    return Start(*options);
  }
  if (options->command == "stop") {
    return Stop(*options);
  }
  if (options->command == "status") {
    return Status(*options);
  }
  if (options->command == "attach") {
    return Attach(*options, "/proc/self/exe");
  }
  return Usage();
#else
  (void)argc;
  (void)argv;
  std::cerr << "microide-server is POSIX-only\n";
  return 1;
#endif
}
