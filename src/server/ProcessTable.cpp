#include "server/ProcessTable.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <utility>

#include "util/Log.h"
#include "util/PosixPipe.h"
#include "util/WakePipe.h"

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#endif

extern char** environ;

namespace microide::server {
namespace {

namespace remote = project::remote;

constexpr std::size_t kMaxArgs = 4096;
constexpr std::size_t kMaxArgvBytes = 1024 * 1024;
constexpr std::size_t kMaxEnv = 256;
constexpr std::size_t kChunkBytes = 64 * 1024;
// After the process exits, how long its pipes may stay open (a grandchild holding
// stdout) before the exit is reported anyway.
constexpr auto kExitDrainGrace = std::chrono::seconds(5);
// How long a terminated process gets between SIGTERM and SIGKILL.
constexpr auto kKillGrace = std::chrono::seconds(2);

bool ValidEnvName(std::string_view name) {
  if (name.empty() || name.size() > 256 || (std::isdigit(static_cast<unsigned char>(name[0])) != 0)) {
    return false;
  }
  return std::all_of(name.begin(), name.end(), [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
  });
}

#if defined(__unix__) || defined(__APPLE__)
void SetNonBlocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
}

// Resolve argv[0] through PATH in the parent: the child of a multi-threaded process
// may only call async-signal-safe functions, and execve is one; execvp is not.
std::string ResolveExecutable(
    const std::string& name,
    const std::vector<std::pair<std::string, std::optional<std::string>>>& env) {
  if (name.find('/') != std::string::npos) {
    return name;
  }
  std::string path_value;
  for (const auto& [key, value] : env) {
    if (key == "PATH" && value.has_value()) {
      path_value = *value;
    }
  }
  if (path_value.empty()) {
    if (const char* path = std::getenv("PATH"); path != nullptr) {
      path_value = path;
    } else {
      path_value = "/usr/local/bin:/usr/bin:/bin";
    }
  }
  std::size_t start = 0;
  while (start <= path_value.size()) {
    const std::size_t end = std::min(path_value.find(':', start), path_value.size());
    std::string dir = path_value.substr(start, end - start);
    if (dir.empty()) {
      dir = ".";
    }
    const std::string candidate = dir + "/" + name;
    struct stat st{};
    if (::stat(candidate.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
        ::access(candidate.c_str(), X_OK) == 0) {
      return candidate;
    }
    start = end + 1;
  }
  return name;  // execve fails with ENOENT; the child exits 127 like a shell would
}
#endif

}  // namespace

ProcessTable::ProcessTable(Sink sink, Limits limits) : sink_(std::move(sink)), limits_(limits) {
  wake_.Open();
  thread_ = std::thread([this]() { Run(); });
}

ProcessTable::~ProcessTable() {
  stop_.store(true, std::memory_order_release);
  wake_.Wake();
  if (thread_.joinable()) {
    thread_.join();
  }
#if defined(__unix__) || defined(__APPLE__)
  std::lock_guard lock(mutex_);
  for (auto& [handle, process] : processes_) {
    (void)handle;
    if (!process->exited && process->pid > 0) {
      ::kill(-process->pid, SIGKILL);
      ::waitpid(process->pid, nullptr, 0);
    }
    for (int fd : {process->stdin_fd, process->out.fd, process->err.fd}) {
      if (fd >= 0) {
        ::close(fd);
      }
    }
  }
#endif
}

std::optional<ProcessTable::SpawnRequest> ProcessTable::ParseSpawn(const util::JsonValue& params,
                                                                   std::string* error) {
  if (!params.IsObject()) {
    *error = "proc/spawn params must be an object";
    return std::nullopt;
  }
  SpawnRequest request;
  const util::JsonValue& argv = params["argv"];
  if (!argv.IsArray() || argv.AsArray().empty() || argv.AsArray().size() > kMaxArgs) {
    *error = "argv must be a non-empty array of at most 4096 strings";
    return std::nullopt;
  }
  std::size_t bytes = 0;
  for (const util::JsonValue& arg : argv.AsArray()) {
    if (!arg.IsString() || arg.AsString().find('\0') != std::string::npos) {
      *error = "every argv element must be a string without NUL bytes";
      return std::nullopt;
    }
    bytes += arg.AsString().size();
    request.argv.push_back(arg.AsString());
  }
  if (bytes > kMaxArgvBytes || request.argv.front().empty()) {
    *error = "argv is too large, or argv[0] is empty";
    return std::nullopt;
  }
  const util::JsonValue& cwd = params["cwd"];
  if (!cwd.IsNull()) {
    if (!cwd.IsString() || cwd.AsString().empty() || cwd.AsString().front() != '/' ||
        cwd.AsString().find('\0') != std::string::npos) {
      *error = "cwd must be an absolute host path";
      return std::nullopt;
    }
    request.cwd = cwd.AsString();
  }
  const util::JsonValue& env = params["env"];
  if (!env.IsNull()) {
    if (!env.IsObject() || env.AsObject().size() > kMaxEnv) {
      *error = "env must be an object of at most 256 entries";
      return std::nullopt;
    }
    for (const auto& entry : env.AsObject()) {
      if (!ValidEnvName(entry.key) || !(entry.value.IsString() || entry.value.IsNull()) ||
          entry.value.AsString().find('\0') != std::string::npos) {
        *error = "env entries must be NAME: string (set) or NAME: null (unset)";
        return std::nullopt;
      }
      request.env.emplace_back(entry.key, entry.value.IsNull()
                                              ? std::nullopt
                                              : std::optional<std::string>(entry.value.AsString()));
    }
  }
  request.keep_on_detach = params["keep_on_detach"].AsBool(false);
  return request;
}

ProcessTable::SpawnResult ProcessTable::Spawn(std::uint64_t connection, SpawnRequest request) {
#if defined(__unix__) || defined(__APPLE__)
  {
    std::lock_guard lock(mutex_);
    if (processes_.size() >= limits_.max_processes) {
      return SpawnResult{.error = "too many processes on this server"};
    }
  }
  // Everything the child needs is built before fork: after it, only
  // async-signal-safe calls.
  const std::string executable = ResolveExecutable(request.argv.front(), request.env);
  std::vector<char*> argv;
  for (std::string& arg : request.argv) {
    argv.push_back(arg.data());
  }
  argv.push_back(nullptr);
  std::vector<std::string> env_storage;
  for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
    const std::string_view text(*entry);
    const std::string_view name = text.substr(0, text.find('='));
    const bool overridden = std::any_of(request.env.begin(), request.env.end(),
                                        [&](const auto& kv) { return kv.first == name; });
    if (!overridden) {
      env_storage.emplace_back(text);
    }
  }
  for (const auto& [key, value] : request.env) {
    if (value.has_value()) {
      env_storage.push_back(key + "=" + *value);
    }
  }
  std::vector<char*> envp;
  for (std::string& entry : env_storage) {
    envp.push_back(entry.data());
  }
  envp.push_back(nullptr);

  int in_pipe[2] = {-1, -1};
  int out_pipe[2] = {-1, -1};
  int err_pipe[2] = {-1, -1};
  if (!util::MakeCloexecPipe(in_pipe) || !util::MakeCloexecPipe(out_pipe) ||
      !util::MakeCloexecPipe(err_pipe)) {
    for (int fd : {in_pipe[0], in_pipe[1], out_pipe[0], out_pipe[1], err_pipe[0], err_pipe[1]}) {
      if (fd >= 0) {
        ::close(fd);
      }
    }
    return SpawnResult{.error = std::string("pipe: ") + std::strerror(errno)};
  }
  const char* cwd = request.cwd.empty() ? nullptr : request.cwd.c_str();
  const pid_t pid = ::fork();
  if (pid == 0) {
    ::setpgid(0, 0);  // its own group: a signal reaches its children too
    ::dup2(in_pipe[0], 0);
    ::dup2(out_pipe[1], 1);
    ::dup2(err_pipe[1], 2);
    struct sigaction dfl{};
    dfl.sa_handler = SIG_DFL;
    ::sigaction(SIGPIPE, &dfl, nullptr);
    sigset_t none;
    ::sigemptyset(&none);
    ::sigprocmask(SIG_SETMASK, &none, nullptr);
    if (cwd != nullptr && ::chdir(cwd) != 0) {
      ::_exit(127);
    }
    ::execve(executable.c_str(), argv.data(), envp.data());
    ::_exit(errno == ENOENT ? 127 : 126);
  }
  // The parent makes the group too (as shells do): every signal goes to the group
  // (kill(-pid)), and one sent before the child reached its own setpgid would name
  // a group that does not exist yet and be lost. After the child's exec this fails
  // with EACCES, harmlessly: the child made the group itself by then.
  if (pid > 0) {
    ::setpgid(pid, pid);
  }
  ::close(in_pipe[0]);
  ::close(out_pipe[1]);
  ::close(err_pipe[1]);
  if (pid < 0) {
    ::close(in_pipe[1]);
    ::close(out_pipe[0]);
    ::close(err_pipe[0]);
    return SpawnResult{.error = std::string("fork: ") + std::strerror(errno)};
  }
  SetNonBlocking(in_pipe[1]);
  SetNonBlocking(out_pipe[0]);
  SetNonBlocking(err_pipe[0]);
  auto process = std::make_unique<Process>();
  process->pid = pid;
  process->stdin_fd = in_pipe[1];
  process->out.fd = out_pipe[0];
  process->err.fd = err_pipe[0];
  process->keep_on_detach = request.keep_on_detach;
  process->connection = connection;
  process->git = std::filesystem::path(request.argv.front()).filename() == "git";
  process->cwd = request.cwd;
  std::uint64_t handle = 0;
  {
    std::lock_guard lock(mutex_);
    handle = next_handle_++;
    process->handle = handle;
    processes_.emplace(handle, std::move(process));
  }
  wake_.Wake();
  return SpawnResult{.handle = handle, .pid = pid};
#else
  (void)connection;
  (void)request;
  return SpawnResult{.error = "not supported"};
#endif
}

bool ProcessTable::WriteStdin(std::uint64_t handle, std::string_view bytes) {
#if defined(__unix__) || defined(__APPLE__)
  std::lock_guard lock(mutex_);
  const auto it = processes_.find(handle);
  if (it == processes_.end() || it->second->stdin_fd < 0) {
    return false;
  }
  // Blocking-ish write of a frame's worth (<= 64 KiB). The pipe is non-blocking; a
  // full pipe waits briefly for the process to read rather than dropping input.
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const ssize_t n = ::write(it->second->stdin_fd, bytes.data() + offset, bytes.size() - offset);
    if (n > 0) {
      offset += static_cast<std::size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      pollfd fd{it->second->stdin_fd, POLLOUT, 0};
      if (::poll(&fd, 1, 1000) <= 0) {
        return false;
      }
      continue;
    }
    return false;
  }
  return true;
#else
  (void)handle;
  (void)bytes;
  return false;
#endif
}

bool ProcessTable::CloseStdin(std::uint64_t handle) {
#if defined(__unix__) || defined(__APPLE__)
  std::lock_guard lock(mutex_);
  const auto it = processes_.find(handle);
  if (it == processes_.end() || it->second->stdin_fd < 0) {
    return false;
  }
  ::close(it->second->stdin_fd);
  it->second->stdin_fd = -1;
  return true;
#else
  (void)handle;
  return false;
#endif
}

bool ProcessTable::Signal(std::uint64_t handle, int signal) {
#if defined(__unix__) || defined(__APPLE__)
  std::lock_guard lock(mutex_);
  const auto it = processes_.find(handle);
  if (it == processes_.end() || it->second->exited) {
    return false;
  }
  return ::kill(-it->second->pid, signal) == 0;
#else
  (void)handle;
  (void)signal;
  return false;
#endif
}

void ProcessTable::Ack(std::uint64_t handle, std::uint64_t stdout_offset,
                       std::uint64_t stderr_offset) {
  {
    std::lock_guard lock(mutex_);
    const auto it = processes_.find(handle);
    if (it == processes_.end()) {
      return;
    }
    for (auto [stream, offset] : {std::pair{&it->second->out, stdout_offset},
                                  std::pair{&it->second->err, stderr_offset}}) {
      // Only what was actually sent can be acknowledged; a lying ack is clamped.
      const std::uint64_t acked = std::min(offset, stream->sent);
      if (acked > stream->base) {
        stream->pending.erase(0, static_cast<std::size_t>(acked - stream->base));
        stream->base = acked;
      }
    }
  }
  wake_.Wake();
}

bool ProcessTable::Attach(std::uint64_t connection, std::uint64_t handle,
                          std::uint64_t stdout_offset, std::uint64_t stderr_offset,
                          std::string* error) {
  {
    std::lock_guard lock(mutex_);
    const auto it = processes_.find(handle);
    if (it == processes_.end()) {
      *error = "no process with that handle";
      return false;
    }
    Process& process = *it->second;
    for (auto [stream, offset] : {std::pair{&process.out, stdout_offset},
                                  std::pair{&process.err, stderr_offset}}) {
      if (offset < stream->base || offset > stream->total()) {
        *error = "offset outside the output this server still holds";
        return false;
      }
    }
    for (auto [stream, offset] : {std::pair{&process.out, stdout_offset},
                                  std::pair{&process.err, stderr_offset}}) {
      stream->pending.erase(0, static_cast<std::size_t>(offset - stream->base));
      stream->base = offset;
      stream->sent = offset;
    }
    process.connection = connection;
    process.exit_sent = false;
  }
  wake_.Wake();
  return true;
}

void ProcessTable::Release(std::uint64_t handle) {
#if defined(__unix__) || defined(__APPLE__)
  std::lock_guard lock(mutex_);
  const auto it = processes_.find(handle);
  if (it == processes_.end() || !it->second->exited) {
    return;
  }
  for (int fd : {it->second->stdin_fd, it->second->out.fd, it->second->err.fd}) {
    if (fd >= 0) {
      ::close(fd);
    }
  }
  processes_.erase(it);
#else
  (void)handle;
#endif
}

void ProcessTable::Detach(std::uint64_t connection) {
#if defined(__unix__) || defined(__APPLE__)
  {
    std::lock_guard lock(mutex_);
    for (auto& [handle, process] : processes_) {
      (void)handle;
      if (process->connection != connection) {
        continue;
      }
      process->connection = 0;
      // Unacknowledged output is resent from the reattaching client's offset.
      process->out.sent = process->out.base;
      process->err.sent = process->err.base;
      if (!process->keep_on_detach && !process->exited) {
        ::kill(-process->pid, SIGTERM);
        // SIGKILL after a grace period, from the table's loop — never from a timer
        // that could outlive the process and signal a reused group id.
        process->kill_at = std::chrono::steady_clock::now() + kKillGrace;
      }
    }
  }
  wake_.Wake();
#else
  (void)connection;
#endif
}

std::size_t ProcessTable::LiveCount() const {
  std::lock_guard lock(mutex_);
  return static_cast<std::size_t>(std::count_if(processes_.begin(), processes_.end(),
                                                [](const auto& p) { return !p.second->exited; }));
}

std::size_t ProcessTable::CountFor(std::uint64_t connection) const {
  std::lock_guard lock(mutex_);
  return static_cast<std::size_t>(std::count_if(
      processes_.begin(), processes_.end(),
      [connection](const auto& p) { return p.second->connection == connection; }));
}

bool ProcessTable::Readable(const Stream& stream) const {
  return stream.fd >= 0 && !stream.eof && stream.pending.size() < limits_.retention_bytes;
}

void ProcessTable::Pump(Process& process, std::vector<Outgoing>& outgoing) {
  if (process.connection == 0) {
    return;
  }
  for (auto [stream, type] : {std::pair{&process.out, remote::FrameType::ProcStdout},
                              std::pair{&process.err, remote::FrameType::ProcStderr}}) {
    while (stream->sent < stream->total() &&
           stream->sent - stream->base < limits_.credit_bytes) {
      const std::size_t room = limits_.credit_bytes - static_cast<std::size_t>(stream->sent - stream->base);
      const std::size_t length = std::min({kChunkBytes, room,
                                           static_cast<std::size_t>(stream->total() - stream->sent)});
      Outgoing frame;
      frame.connection = process.connection;
      frame.type = type;
      frame.handle = process.handle;
      frame.bytes = stream->pending.substr(static_cast<std::size_t>(stream->sent - stream->base), length);
      outgoing.push_back(std::move(frame));
      stream->sent += length;
    }
  }
  const bool drained = process.out.sent == process.out.total() &&
                       process.err.sent == process.err.total();
  if (process.exited && !process.exit_sent && drained &&
      ((process.out.eof || process.out.fd < 0) && (process.err.eof || process.err.fd < 0))) {
    util::JsonObject params;
    params["handle"] = util::JsonValue(static_cast<std::int64_t>(process.handle));
    if (process.exit_signal != 0) {
      params["signal"] = util::JsonValue(static_cast<std::int64_t>(process.exit_signal));
    } else {
      params["code"] = util::JsonValue(static_cast<std::int64_t>(process.exit_code));
    }
    params["stdout_total"] = util::JsonValue(static_cast<std::int64_t>(process.out.total()));
    params["stderr_total"] = util::JsonValue(static_cast<std::int64_t>(process.err.total()));
    Outgoing exit;
    exit.connection = process.connection;
    exit.is_exit = true;
    if (process.git) {
      exit.git_cwd = process.cwd;
    }
    exit.handle = process.handle;
    exit.params = util::JsonValue(std::move(params));
    outgoing.push_back(std::move(exit));
    process.exit_sent = true;
  }
}

void ProcessTable::Flush(std::vector<Outgoing>& outgoing) {
  for (Outgoing& item : outgoing) {
    if (item.is_exit) {
      if (item.git_cwd.has_value() && sink_.git_exited) {
        util::JsonObject params = item.params.AsObject();
        params["git_generation"] =
            util::JsonValue(static_cast<std::int64_t>(sink_.git_exited(*item.git_cwd)));
        item.params = util::JsonValue(std::move(params));
      }
      if (sink_.notify) {
        sink_.notify(item.connection, "proc/exit", item.handle, item.params);
      }
    } else if (sink_.content) {
      sink_.content(item.connection, item.type, item.handle, item.bytes);
    }
  }
  outgoing.clear();
}

void ProcessTable::Reap() {
#if defined(__unix__) || defined(__APPLE__)
  for (auto& [handle, process] : processes_) {
    (void)handle;
    if (process->exited) {
      continue;
    }
    int status = 0;
    const pid_t done = ::waitpid(process->pid, &status, WNOHANG);
    if (done != process->pid) {
      continue;
    }
    process->exited = true;
    if (WIFSIGNALED(status)) {
      process->exit_signal = WTERMSIG(status);
    } else {
      process->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    if (process->stdin_fd >= 0) {
      ::close(process->stdin_fd);
      process->stdin_fd = -1;
    }
  }
#endif
}

void ProcessTable::Run() {
#if defined(__unix__) || defined(__APPLE__)
  std::map<std::uint64_t, std::chrono::steady_clock::time_point> exited_at;
  std::vector<Outgoing> outgoing;
  std::vector<pollfd> fds;
  std::vector<std::pair<std::uint64_t, bool>> owners;  // handle, is_stderr
  while (!stop_.load(std::memory_order_acquire)) {
    fds.clear();
    owners.clear();
    fds.push_back(pollfd{wake_.read_fd(), POLLIN, 0});
    bool any_running = false;
    {
      std::lock_guard lock(mutex_);
      Reap();
      const auto now = std::chrono::steady_clock::now();
      for (auto it = processes_.begin(); it != processes_.end();) {
        Process& process = *it->second;
        any_running = any_running || !process.exited;
        if (!process.exited && process.kill_at.has_value() && now >= *process.kill_at) {
          ::kill(-process.pid, SIGKILL);
          process.kill_at.reset();
        }
        if (process.exited) {
          auto [mark, inserted] = exited_at.try_emplace(process.handle, now);
          // A grandchild holding the pipes open must not hold the exit back forever.
          if (!inserted && now - mark->second > kExitDrainGrace) {
            for (Stream* stream : {&process.out, &process.err}) {
              if (stream->fd >= 0) {
                ::close(stream->fd);
                stream->fd = -1;
                stream->eof = true;
              }
            }
          }
        }
        Pump(process, outgoing);
        // Nobody will ever collect an exited, unkept process whose client is gone.
        if (process.exited && process.connection == 0 && !process.keep_on_detach) {
          for (int fd : {process.stdin_fd, process.out.fd, process.err.fd}) {
            if (fd >= 0) {
              ::close(fd);
            }
          }
          exited_at.erase(process.handle);
          it = processes_.erase(it);
          continue;
        }
        for (auto [stream, is_err] : {std::pair{&process.out, false}, std::pair{&process.err, true}}) {
          if (Readable(*stream)) {
            fds.push_back(pollfd{stream->fd, POLLIN, 0});
            owners.emplace_back(process.handle, is_err);
          }
        }
        ++it;
      }
    }
    Flush(outgoing);
    // No portable child-exit descriptor: while anything runs, re-check every 50 ms.
    const int timeout_ms = any_running ? 50 : (exited_at.empty() ? -1 : 500);
    if (::poll(fds.data(), fds.size(), timeout_ms) < 0 && errno != EINTR) {
      util::Log("process table: poll failed");
      return;
    }
    if ((fds[0].revents & POLLIN) != 0) {
      wake_.Drain();
    }
    std::lock_guard lock(mutex_);
    for (std::size_t i = 1; i < fds.size(); ++i) {
      if ((fds[i].revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
        continue;
      }
      const auto it = processes_.find(owners[i - 1].first);
      if (it == processes_.end()) {
        continue;
      }
      Stream& stream = owners[i - 1].second ? it->second->err : it->second->out;
      char buffer[kChunkBytes];
      const ssize_t n = ::read(stream.fd, buffer, sizeof(buffer));
      if (n > 0) {
        stream.pending.append(buffer, static_cast<std::size_t>(n));
      } else if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
        ::close(stream.fd);
        stream.fd = -1;
        stream.eof = true;
      }
    }
  }
#endif
}

}  // namespace microide::server
