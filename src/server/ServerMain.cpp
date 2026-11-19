// microide-server: the host half of a remote project (dev-docs/design/remote-projects.md
// § 6.6). One daemon per user; subcommands:
//
//   start   [--on-demand] [--socket-dir D] [--idle-timeout-ms N] [--keep-grace-ms N]   daemonize and serve
//   stop    [--socket-dir D]                                       ask it to exit
//   status  [--socket-dir D] [--json]                              print what it serves:
//                                                                  a table, or the raw reply
//   attach  [--socket-dir D] [--idle-timeout-ms N]                 relay stdio to it,
//                                                                  starting one on demand
//   serve-stdio                                                    serve ONE connection on
//                                                                  stdin/stdout (tests)
//   open-url URL                                                   ask the editor attached
//                                                                  to this terminal to open
//                                                                  URL (also: run as the
//                                                                  `microide-browser` link,
//                                                                  the shells' $BROWSER)
//   edit [+LINE] FILE                                              have that editor edit
//                                                                  FILE, and wait for its
//                                                                  tab to close (also: the
//                                                                  `microide-edit` link,
//                                                                  the shells' $EDITOR)
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

#include "platform/OpenFileLimit.h"
#include "platform/UnixSocket.h"
#include "project/remote/RemotePeer.h"
#include "project/remote/RemoteProtocol.h"
#include "project/remote/RemoteServerPaths.h"
#include "project/remote/ServerStatusText.h"
#include "server/RemoteServer.h"
#include "terminal/TerminalHostWire.h"
#include "util/JsonFormat.h"
#include "util/JsonValue.h"
#include "util/Log.h"
#include "util/Parse.h"
#include "util/PosixFd.h"
#include "util/StringUtil.h"
#include "util/TextFileIO.h"

#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

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
  std::chrono::milliseconds keep_grace{30 * 60 * 1000};
  bool json = false;
};

int Usage() {
  std::cerr << "usage: microide-server <start|stop|status|attach|serve-stdio> "
               "[--socket-dir DIR] [--on-demand] [--idle-timeout-ms N] [--keep-grace-ms N] [--json]\n";
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
    } else if (arg == "--json") {
      options.json = true;
    } else if (arg == "--idle-timeout-ms" && i + 1 < argc) {
      const auto ms = microide::util::ParseInt64(argv[++i]);
      if (!ms.has_value() || *ms < 0) {
        return std::nullopt;
      }
      options.idle_timeout = std::chrono::milliseconds(*ms);
    } else if (arg == "--keep-grace-ms" && i + 1 < argc) {
      const auto ms = microide::util::ParseInt64(argv[++i]);
      if (!ms.has_value() || *ms < 0) {
        return std::nullopt;
      }
      options.keep_grace = std::chrono::milliseconds(*ms);
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
    // One daemon per socket directory. Two `attach`es that find no daemon answering
    // -- two projects reaching one fresh host at once -- both start one, and
    // ListenUnixSocket replaces whatever socket of ours is at the path, live or
    // not: the second daemon took the path from the first, the projects were split
    // between them, and every later connection reached only the second
    // (TD-2026-10-28-414). The lock is held for the daemon's life (the kernel drops
    // it at exit, so it is never stale) and taken before the socket is touched. A
    // loser leaves when the socket answers; else the holder is on its idle way out
    // and the lock frees within moments.
    const microide::util::UniqueFd lock(
        ::open((options.socket_dir / "server.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600));
    bool locked = false;
    for (int attempt = 0; lock.IsValid() && attempt < 500 && !locked; ++attempt) {
      locked = ::flock(lock.Get(), LOCK_EX | LOCK_NB) == 0;
      if (!locked) {
        if (Connectable(socket)) {
          ::_exit(0);  // another daemon serves it
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    }
    if (!locked) {
      microide::util::Log(lock.IsValid() ? "another daemon holds the server lock and does not answer"
                                         : "cannot open the server lock: " + std::string(std::strerror(errno)));
      ::_exit(1);
    }
    // A daemon of a release before the lock serves without it.
    if (Connectable(socket)) {
      ::_exit(0);
    }
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
          .keep_grace = options.keep_grace,
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

// One request to the running server, synchronously; for status and stop. A zero
// `limit` waits for as long as the server takes (term/edit: a person is typing).
std::optional<microide::util::JsonValue> Ask(const Options& options, std::string_view method,
                                             std::string* error,
                                             microide::util::JsonValue params = microide::util::JsonValue(nullptr),
                                             std::chrono::seconds limit = std::chrono::seconds(10)) {
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
  peer.Request(method, params, remote::Lane::Interactive,
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
  if (limit.count() == 0) {
    done.wait(lock, [&]() { return finished; });
  } else if (!done.wait_for(lock, limit, [&]() { return finished; })) {
    *error = "the server did not answer";
  }
  lock.unlock();
  peer.Stop();
  return answer;
}

int Status(const Options& options) {
  std::string error;
  if (options.json) {
    // The plain reply, exactly what a client's server/status returns: no figures,
    // so nothing reads /proc.
    const auto status = Ask(options, remote::method::kServerStatus, &error);
    if (!status.has_value()) {
      std::cerr << "microide-server: " << error << '\n';
      return 1;
    }
    const auto formatted = microide::util::FormatJson(microide::util::SerializeJson(*status), "  ");
    std::cout << (formatted.ok ? formatted.text : microide::util::SerializeJson(*status)) << '\n';
    return 0;
  }
  // A CPU share is a difference between two samples: take the first, wait, and
  // print the second, so the column is never "not reported" for want of one.
  microide::util::JsonObject params;
  params["activity"] = microide::util::JsonValue(true);
  const microide::util::JsonValue ask(std::move(params));
  if (!Ask(options, remote::method::kServerStatus, &error, ask).has_value()) {
    std::cerr << "microide-server: " << error << '\n';
    return 1;
  }
  std::this_thread::sleep_for(std::chrono::seconds(1));
  const auto status = Ask(options, remote::method::kServerStatus, &error, ask);
  if (!status.has_value()) {
    std::cerr << "microide-server: " << error << '\n';
    return 1;
  }
  std::cout << remote::FormatServerStatusTable(remote::ServerStatusFromJson(*status));
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
      const std::string keep = std::to_string(options.keep_grace.count());
      const std::string dir = options.socket_dir.string();
      ::execl(self, self, "start", "--on-demand", "--socket-dir", dir.c_str(), "--idle-timeout-ms",
              idle.c_str(), "--keep-grace-ms", keep.c_str(), static_cast<char*>(nullptr));
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

}  // namespace

// The BROWSER of the server's terminals (RemoteServer::InstallBrowserForwarder):
// hand `url` to the editor attached to the terminal this runs in, which opens it
// on the user's machine -- what VS Code's remote terminals do, so `claude` or
// `gh auth login` opens the page where the user is rather than failing to find a
// display on the host. Exit 0 once an editor has it; 1 when none can take it (no
// terminal of ours, nobody attached), so the program falls back to printing it.
static int OpenUrl(std::string_view url) {
  const char* handle_text = std::getenv("MICROIDE_TERMINAL_HANDLE");
  const std::optional<std::int64_t> handle =
      handle_text != nullptr ? microide::util::ParseInt64(handle_text) : std::nullopt;
  if (!handle.has_value() || *handle <= 0) {
    std::cerr << "microide-server: not in a microide terminal; cannot open " << url << '\n';
    return 1;
  }
  Options options;
  if (const char* dir = std::getenv("MICROIDE_SERVER_SOCKET_DIR"); dir != nullptr && *dir != '\0') {
    options.socket_dir = dir;
  } else if (const auto home = HomeDirectory(); !home.empty()) {
    options.socket_dir = remote::DefaultServerSocketDir(home);
  }
  microide::util::JsonObject params;
  params["handle"] = microide::util::JsonValue(*handle);
  params["url"] = microide::util::JsonValue(std::string(url));
  std::string error;
  const auto answer = Ask(options, remote::method::kTermOpenUrl, &error,
                          microide::util::JsonValue(std::move(params)));
  if (!answer.has_value() || !(*answer)["delivered"].AsBool()) {
    std::cerr << "microide-server: no editor is attached to this terminal to open " << url
              << (error.empty() ? std::string() : ": " + error) << '\n';
    return 1;
  }
  return 0;
}

// The terminal's own editor, with the helper's arguments (`arguments[0]` is
// replaced by its name): what the program would have run had the server not set
// EDITOR. Never returns.
[[noreturn]] static void RunTerminalEditor(char** arguments) {
  // sensible-editor reads these, and would run this helper again.
  for (const char* variable : {"VISUAL", "EDITOR"}) {
    if (const char* value = std::getenv(variable);
        value != nullptr && std::filesystem::path(value).filename() == remote::kEditorHelperName) {
      ::unsetenv(variable);
    }
  }
  for (const char* editor : {"sensible-editor", "editor", "vi", "nano"}) {
    arguments[0] = const_cast<char*>(editor);
    ::execvp(editor, arguments);
  }
  std::cerr << "microide-edit: no editor is attached to this terminal, and it has no editor of its own\n";
  ::_exit(127);
}

// The EDITOR of the server's terminals (RemoteServer::InstallTerminalForwarders):
// hand the file to the editor attached to the terminal this runs in, wait for its
// tab to close and leave what was saved there in the file -- `code --wait`, for a
// commit message, a rebase list or an agent CLI's prompt. `arguments` are the
// program's (`[+LINE] FILE`), after a slot for the editor's name. Anything this
// cannot serve -- no editor attached, an older one, several files, a file that is
// not UTF-8 text of at most 1 MiB -- runs the terminal's own editor instead.
static int EditFile(char** arguments) {
  std::int64_t line = 0;
  const char* file = nullptr;
  bool options_ended = false;
  for (char** argument = arguments + 1; *argument != nullptr; ++argument) {
    const std::string_view text(*argument);
    if (!options_ended && text.size() > 1 && text.front() == '+') {
      line = microide::util::ParseInt64(text.substr(1)).value_or(0);
    } else if (!options_ended && (text == "--wait" || text == "-w")) {
      // What this does anyway.
    } else if (!options_ended && text == "--") {
      options_ended = true;
    } else if (file == nullptr && (options_ended || text.empty() || text.front() != '-')) {
      file = *argument;
    } else {
      RunTerminalEditor(arguments);
    }
  }
  const char* handle_text = std::getenv("MICROIDE_TERMINAL_HANDLE");
  const std::optional<std::int64_t> handle =
      handle_text != nullptr ? microide::util::ParseInt64(handle_text) : std::nullopt;
  if (file == nullptr || !handle.has_value() || *handle <= 0) {
    RunTerminalEditor(arguments);
  }
  std::error_code status_error;
  const std::filesystem::path path = std::filesystem::absolute(file, status_error).lexically_normal();
  const std::filesystem::file_status status = std::filesystem::status(path, status_error);
  const bool exists = std::filesystem::exists(status);
  std::string original;
  if (exists) {
    std::error_code size_error;
    std::optional<std::string> read;
    if (std::filesystem::is_regular_file(status) &&
        std::filesystem::file_size(path, size_error) <= microide::terminal::kMaxHostEditBytes && !size_error) {
      read = microide::util::ReadTextFile(path);
    }
    if (!read.has_value() || read->size() > microide::terminal::kMaxHostEditBytes || !microide::util::IsValidUtf8(*read)) {
      RunTerminalEditor(arguments);
    }
    original = std::move(*read);
  }
  Options options;
  if (const char* dir = std::getenv("MICROIDE_SERVER_SOCKET_DIR"); dir != nullptr && *dir != '\0') {
    options.socket_dir = dir;
  } else if (const auto home = HomeDirectory(); !home.empty()) {
    options.socket_dir = remote::DefaultServerSocketDir(home);
  }
  microide::util::JsonObject params;
  params["handle"] = microide::util::JsonValue(*handle);
  params["path"] = microide::util::JsonValue(path.string());
  params["line"] = microide::util::JsonValue(std::max<std::int64_t>(line, 0));
  params["text"] = microide::util::JsonValue(original);
  std::string error;
  const auto answer = Ask(options, remote::method::kTermEdit, &error,
                          microide::util::JsonValue(std::move(params)), std::chrono::seconds(0));
  if (!answer.has_value()) {
    std::cerr << "microide-edit: " << path.string() << " was not edited: "
              << (error.empty() ? std::string("the server went away") : error) << '\n';
    return 1;
  }
  if (!(*answer)["delivered"].AsBool()) {
    RunTerminalEditor(arguments);
  }
  const microide::util::JsonValue& text = (*answer)["text"];
  if (!text.IsString() || (exists && text.AsString() == original) || (!exists && text.AsString().empty())) {
    return 0;  // closed as it was: nothing to write, as leaving vi with :q
  }
  // In place, as an editor writes: the file keeps its inode, mode and links.
  const microide::util::UniqueFd out(::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666));
  const std::string& bytes = text.AsString();
  std::size_t written = 0;
  while (out.IsValid() && written < bytes.size()) {
    const ssize_t count = ::write(out.Get(), bytes.data() + written, bytes.size() - written);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      break;
    }
    written += static_cast<std::size_t>(count);
  }
  if (!out.IsValid() || written != bytes.size()) {
    std::cerr << "microide-edit: cannot write " << path.string() << ": " << std::strerror(errno) << '\n';
    return 1;
  }
  return 0;
}

int main(int argc, char** argv) {
  // Before any descriptor exists: a closed 0/1/2 would be handed to the first pipe
  // or socket (and the daemon's own stdio detach below assumes they are open).
  microide::util::EnsureStandardDescriptorsOpen();
  // Before any thread: every remote terminal and job spends descriptors
  // (platform/OpenFileLimit.h).
  microide::platform::RaiseOpenFileLimit();
  ::signal(SIGPIPE, SIG_IGN);
  if (argc >= 1 && std::filesystem::path(argv[0]).filename() == remote::kBrowserHelperName) {
    return argc == 2 ? OpenUrl(argv[1]) : (std::cerr << "usage: microide-browser URL\n", 2);
  }
  if (argc == 3 && std::string_view(argv[1]) == "open-url") {
    return OpenUrl(argv[2]);
  }
  if (argc >= 1 && std::filesystem::path(argv[0]).filename() == remote::kEditorHelperName) {
    return EditFile(argv);
  }
  if (argc >= 2 && std::string_view(argv[1]) == "edit") {
    return EditFile(argv + 1);
  }
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
}
