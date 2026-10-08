#include "project/remote/RemoteProcessLauncher.h"

#include "project/remote/RemoteConnection.h"
#include "project/remote/RemoteTerminalChannel.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <thread>
#include <utility>

#include "platform/AsyncSubprocess.h"
#include "util/PosixPipe.h"
#include "util/WakePipe.h"

#if defined(__unix__) || defined(__APPLE__)
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

namespace microide::project::remote {
namespace {

// The local subprocess layer's capture ceiling, kept identical so a remote run
// truncates exactly where a local one would.
constexpr std::size_t kMaxCaptureBytes = 128ull * 1024 * 1024;

std::vector<std::pair<std::string, std::optional<std::string>>> EnvOf(
    const platform::SubprocessOptions& options) {
  std::vector<std::pair<std::string, std::optional<std::string>>> env;
  for (const auto& override_entry : options.environment_overrides) {
    env.emplace_back(override_entry.name, override_entry.value);
  }
  return env;
}

int ExitStatus(std::optional<int> code, std::optional<int> signal) {
  // As the local layer reports it: a signal is 128 + signo.
  return signal.has_value() ? 128 + *signal : code.value_or(-1);
}

#if defined(__unix__) || defined(__APPLE__)

// A host process materialized locally as two pipes, so AsyncSubprocess — and the
// language server / debug adapter transports above it — drive it exactly like a
// local child. A pump thread moves bytes: what the consumer writes to its stdin
// pipe goes out as proc/stdin; proc/stdout is written into its stdout pipe, and is
// acknowledged only once WRITTEN, so a consumer that stops reading stops the host
// process through the credit window, end to end.
//
// It is spawned KEPT, so a dropped link does not end it on the host: on a
// reconnect (RemoteConnection::Replace) it proc/attaches from the bytes it has
// received, and what the consumer wrote meanwhile is held and sent then. A
// connection gone for good (Disconnect, a host that restarted) ends it.
class RemoteAsyncProcess final : public platform::AsyncProcessController, public Reattachable {
 public:
  RemoteAsyncProcess(std::shared_ptr<RemoteServerClient> client) : client_(std::move(client)) {
    wake_.Open();
  }

  void Reattach(std::shared_ptr<RemoteServerClient> client) override {
    std::shared_ptr<RemoteServerClient> previous;
    std::uint64_t stdout_offset = 0;
    std::uint64_t stderr_offset = 0;
    {
      std::lock_guard lock(mutex_);
      if (exit_status_.has_value() || handle_ == 0) {
        return;
      }
      previous = std::exchange(client_, client);
      stdout_offset = stdout_delivered_ + pending_out_.size();
      stderr_offset = stderr_received_;
    }
    if (client == nullptr) {
      // Gone for good: do not leave it running on the host for nobody.
      if (previous != nullptr) {
        previous->Signal(handle_, "KILL");
        previous->Release(handle_);
      }
      Ended();
      return;
    }
    client->RegisterProcess(handle_, Events());
    std::string error;
    if (!client->AttachProcess(handle_, stdout_offset, stderr_offset, &error)) {
      Ended();  // the host restarted: the consumer sees EOF and starts over
      return;
    }
    std::string held;
    {
      std::lock_guard lock(mutex_);
      held.swap(pending_stdin_);
    }
    if (!held.empty()) {
      client->WriteStdin(handle_, held);
    }
    wake_.Wake();
  }

  ~RemoteAsyncProcess() override {
    stop_.store(true);
    wake_.Wake();
    if (thread_.joinable()) {
      thread_.join();
    }
    for (int fd : {stdin_read_, stdout_write_}) {
      if (fd >= 0) {
        ::close(fd);
      }
    }
    const std::shared_ptr<RemoteServerClient> client = Client();
    if (handle_ != 0 && client != nullptr) {
      if (!exited()) {
        client->Signal(handle_, "KILL");
      }
      client->Release(handle_);
    }
  }

  // The consumer's ends, for AsyncSubprocess::Adopt; nullopt when pipes fail.
  std::optional<std::pair<int, int>> MakePipes() {
    int in[2] = {-1, -1};
    int out[2] = {-1, -1};
    if (!util::MakeCloexecPipe(in, /*nonblocking=*/false) ||
        !util::MakeCloexecPipe(out, /*nonblocking=*/false)) {
      for (int fd : {in[0], in[1], out[0], out[1]}) {
        if (fd >= 0) ::close(fd);
      }
      return std::nullopt;
    }
    stdin_read_ = in[0];
    stdout_write_ = out[1];
    ::fcntl(stdin_read_, F_SETFL, O_NONBLOCK);
    ::fcntl(stdout_write_, F_SETFL, O_NONBLOCK);
    return std::pair{in[1], out[0]};
  }

  RemoteServerClient::ProcessEvents Events() {
    return RemoteServerClient::ProcessEvents{
        .output =
            [this](FrameType stream, std::string_view bytes) {
              std::lock_guard lock(mutex_);
              if (stream == FrameType::ProcStdout) {
                pending_out_.append(bytes);
              } else {
                stderr_received_ += bytes.size();  // a remote server's stderr is not shown
                ack_dirty_ = true;
              }
              wake_.Wake();
            },
        .exit =
            [this](std::optional<int> code, std::optional<int> signal) {
              std::lock_guard lock(mutex_);
              exit_status_ = ExitStatus(code, signal);
              exited_cv_.notify_all();
              wake_.Wake();
            },
    };
  }

  void Begin(std::uint64_t handle, int pid) {
    handle_ = handle;
    pid_ = pid;
    thread_ = std::thread([this]() { Pump(); });
  }

  bool IsRunning() const override { return !exited(); }
  std::optional<int> exit_code() const override {
    std::lock_guard lock(mutex_);
    return exit_status_;
  }
  int pid() const override { return pid_; }
  void Terminate(int timeout_ms) override {
    const std::shared_ptr<RemoteServerClient> client = Client();
    if (exited() || client == nullptr) {
      return;
    }
    client->Signal(handle_, "TERM");
    std::unique_lock lock(mutex_);
    if (!exited_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                             [&]() { return exit_status_.has_value(); })) {
      lock.unlock();
      client->Signal(handle_, "KILL");
      lock.lock();
      exited_cv_.wait_for(lock, std::chrono::seconds(2), [&]() { return exit_status_.has_value(); });
    }
  }

 private:
  bool exited() const {
    std::lock_guard lock(mutex_);
    return exit_status_.has_value();
  }

  std::shared_ptr<RemoteServerClient> Client() const {
    std::lock_guard lock(mutex_);
    return client_;
  }

  // The process is over as far as the consumer is concerned (EOF once its output
  // is delivered), without the host having reported an exit.
  void Ended() {
    {
      std::lock_guard lock(mutex_);
      if (!exit_status_.has_value()) {
        exit_status_ = 128 + 9;  // as a killed process reports
      }
    }
    exited_cv_.notify_all();
    wake_.Wake();
  }

  void Pump() {
    char buffer[64 * 1024];
    while (!stop_.load()) {
      bool want_write = false;
      bool finished = false;
      {
        std::lock_guard lock(mutex_);
        want_write = !pending_out_.empty() && stdout_write_ >= 0;
        // Every byte delivered and the process gone: EOF on the consumer's stdout.
        finished = exit_status_.has_value() && pending_out_.empty();
      }
      if (finished && stdout_write_ >= 0) {
        ::close(stdout_write_);
        stdout_write_ = -1;
      }
      pollfd fds[3] = {{wake_.read_fd(), POLLIN, 0},
                       {stdin_read_, POLLIN, 0},
                       {want_write ? stdout_write_ : -1, POLLOUT, 0}};
      if (::poll(fds, 3, -1) < 0 && errno != EINTR) {
        return;
      }
      if ((fds[0].revents & POLLIN) != 0) {
        wake_.Drain();
      }
      if (stdin_read_ >= 0 && (fds[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
        const ssize_t n = ::read(stdin_read_, buffer, sizeof(buffer));
        if (n > 0) {
          const std::string_view bytes(buffer, static_cast<std::size_t>(n));
          // The link is down: hold it for the reconnect rather than lose a
          // request the consumer will wait on forever.
          const std::shared_ptr<RemoteServerClient> client = Client();
          if (client != nullptr && !client->WriteStdin(handle_, bytes)) {
            std::lock_guard lock(mutex_);
            pending_stdin_.append(bytes);
          }
        } else if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
          ::close(stdin_read_);
          stdin_read_ = -1;
          if (const std::shared_ptr<RemoteServerClient> client = Client()) {
            client->CloseStdin(handle_);
          }
        }
      }
      std::uint64_t ack_out = 0;
      std::uint64_t ack_err = 0;
      bool ack = false;
      {
        std::lock_guard lock(mutex_);
        if (stdout_write_ >= 0 && !pending_out_.empty()) {
          const ssize_t n = ::write(stdout_write_, pending_out_.data(), pending_out_.size());
          if (n > 0) {
            pending_out_.erase(0, static_cast<std::size_t>(n));
            stdout_delivered_ += static_cast<std::uint64_t>(n);
            ack_dirty_ = true;
          } else if (n < 0 && errno == EPIPE) {
            pending_out_.clear();  // the consumer is gone
          }
        }
        if (ack_dirty_) {
          ack_dirty_ = false;
          ack = true;
          ack_out = stdout_delivered_;
          ack_err = stderr_received_;
        }
      }
      if (ack) {
        if (const std::shared_ptr<RemoteServerClient> client = Client()) {
          client->Ack(handle_, ack_out, ack_err);
        }
      }
    }
  }

  std::shared_ptr<RemoteServerClient> client_;  // guarded by mutex_: a reconnect swaps it
  std::uint64_t handle_ = 0;
  int pid_ = -1;
  int stdin_read_ = -1;
  int stdout_write_ = -1;
  util::WakePipe wake_;
  std::thread thread_;
  std::atomic<bool> stop_{false};

  mutable std::mutex mutex_;
  std::condition_variable exited_cv_;
  std::string pending_out_;
  std::string pending_stdin_;  // written while the link was down
  std::uint64_t stdout_delivered_ = 0;
  std::uint64_t stderr_received_ = 0;
  bool ack_dirty_ = false;
  std::optional<int> exit_status_;
};

#endif

}  // namespace

RemoteProcessLauncher::RemoteProcessLauncher(std::shared_ptr<RemoteConnection> connection,
                                             RemotePathMap map, Options options)
    : connection_(std::move(connection)), map_(std::move(map)), options_(std::move(options)) {}

RemoteProcessLauncher::RemoteProcessLauncher(std::shared_ptr<RemoteServerClient> client,
                                             RemotePathMap map, Options options)
    : RemoteProcessLauncher(std::make_shared<RemoteConnection>(std::move(client)), std::move(map),
                            std::move(options)) {}

std::filesystem::path RemoteProcessLauncher::ResolveWorkingDirectory(
    std::filesystem::path cwd) const {
  if (cwd.empty()) {
    return map_.host_root();
  }
  if (std::optional<std::filesystem::path> host = map_.ToHost(cwd)) {
    return *host;
  }
  if (std::optional<std::filesystem::path> host = HostPathOfCached(cwd)) {
    return *host;
  }
  return cwd;
}

std::shared_ptr<terminal::TerminalHostChannel> RemoteProcessLauncher::OpenTerminal(
    const OpenRequest& request, terminal::TerminalSession& session, std::string* error) const {
  OpenRequest host_request = request;
  host_request.credit_bytes = options_.terminal_credit_bytes;
  host_request.prefetch_lines = options_.terminal_prefetch_lines;
  // A directory outside the project's tree has no host counterpart: the host
  // starts the shell in the user's home rather than in a path that only exists
  // here (empty = $HOME). A terminal-only connection has no tree at all.
  host_request.working_directory =
      map_.host_root().empty() ? std::filesystem::path()
                               : map_.ToHost(request.working_directory).value_or(std::filesystem::path());
  auto channel = RemoteTerminalChannel::Open(
      connection_->client(), host_request, session,
      [map = map_](const std::filesystem::path& host_path) {
        return map.ToLocal(host_path).value_or(host_path);
      });
  if (channel == nullptr) {
    if (error != nullptr) {
      *error = "not connected to the host";
    }
    return nullptr;
  }
  connection_->Track(channel);
  return channel;
}

std::filesystem::path RemoteProcessLauncher::LocalPathFromHost(
    std::filesystem::path host_path) const {
  if (std::optional<std::filesystem::path> local = map_.ToLocal(host_path)) {
    return *local;
  }
  // Outside the project: the host's file, never a same-named local one.
  if (!options_.host_file_cache.empty() && host_path.is_absolute()) {
    return options_.host_file_cache / host_path.lexically_normal().relative_path();
  }
  return host_path;
}

std::optional<std::filesystem::path> RemoteProcessLauncher::HostPathOfCached(
    const std::filesystem::path& local_path) const {
  if (options_.host_file_cache.empty()) {
    return std::nullopt;
  }
  const std::filesystem::path relative =
      local_path.lexically_normal().lexically_relative(options_.host_file_cache);
  const std::string text = relative.generic_string();
  if (text.empty() || text == "." || text.rfind("..", 0) == 0) {
    return std::nullopt;
  }
  return std::filesystem::path("/") / relative;
}

platform::SubprocessResult RemoteProcessLauncher::Run(std::vector<std::string> argv,
                                                      platform::SubprocessOptions options) const {
  struct Collected {
    std::mutex mutex;
    std::condition_variable done;
    std::string out;
    std::string err;
    bool truncated = false;
    std::optional<int> status;
  };
  auto collected = std::make_shared<Collected>();
  const bool capture_out = options.capture_stdout;
  const bool capture_err = options.capture_stderr && !options.silence_stderr;
  const std::shared_ptr<RemoteServerClient> client = connection_->client();
  if (client == nullptr || !client->connected()) {
    platform::SubprocessResult result;
    result.exit_code = 127;
    result.stderr_text = "not connected to the host";
    return result;
  }
  // The handle is learned only from the spawn reply, after output may have started
  // arriving; acks go out once it is known.
  auto handle = std::make_shared<std::atomic<std::uint64_t>>(0);
  // Acknowledge by counting what arrived, not what was captured: an uncaptured
  // stream must not stall the process on its credit window.
  auto received = std::make_shared<std::pair<std::atomic<std::uint64_t>, std::atomic<std::uint64_t>>>();
  RemoteServerClient::ProcessEvents events{
      .output =
          [collected, capture_out, capture_err, client, handle, received](FrameType stream,
                                                                          std::string_view bytes) {
            const bool is_out = stream == FrameType::ProcStdout;
            {
              std::lock_guard lock(collected->mutex);
              std::string& target = is_out ? collected->out : collected->err;
              if ((is_out ? capture_out : capture_err)) {
                if (target.size() + bytes.size() <= kMaxCaptureBytes) {
                  target.append(bytes);
                } else if (!collected->truncated) {
                  // As the local layer does at its ceiling: stop the firehose.
                  collected->truncated = true;
                  if (const std::uint64_t h = handle->load(); h != 0) {
                    client->Signal(h, "KILL");
                  }
                }
              }
            }
            (is_out ? received->first : received->second) += bytes.size();
            if (const std::uint64_t h = handle->load(); h != 0) {
              client->Ack(h, received->first.load(), received->second.load());
            }
          },
      .exit =
          [collected](std::optional<int> code, std::optional<int> signal) {
            std::lock_guard lock(collected->mutex);
            collected->status = ExitStatus(code, signal);
            collected->done.notify_all();
          },
  };
  const RemoteServerClient::Spawned spawned = client->Spawn(
      argv, ResolveWorkingDirectory(options.cwd), EnvOf(options), false, std::move(events));
  if (spawned.handle == 0) {
    platform::SubprocessResult result;
    result.exit_code = 127;
    result.stderr_text = spawned.error;
    return result;
  }
  Record(argv);
  handle->store(spawned.handle);
  client->Ack(spawned.handle, received->first.load(), received->second.load());
  if (!options.stdin_text.empty()) {
    client->WriteStdin(spawned.handle, options.stdin_text);
  }
  client->CloseStdin(spawned.handle);

  platform::SubprocessResult result;
  {
    std::unique_lock lock(collected->mutex);
    const auto finished = [&]() { return collected->status.has_value() || !client->connected(); };
    if (options.timeout_ms > 0) {
      if (!collected->done.wait_for(lock, std::chrono::milliseconds(options.timeout_ms), finished)) {
        result.timed_out = true;
      }
    } else {
      while (!collected->done.wait_for(lock, std::chrono::milliseconds(500), finished)) {
      }
    }
  }
  if (result.timed_out) {
    client->Signal(spawned.handle, "KILL");
    std::unique_lock lock(collected->mutex);
    collected->done.wait_for(lock, std::chrono::seconds(5),
                             [&]() { return collected->status.has_value(); });
  }
  {
    std::lock_guard lock(collected->mutex);
    result.stdout_text = std::move(collected->out);
    result.stderr_text = std::move(collected->err);
    result.truncated = collected->truncated;
    result.exit_code = result.timed_out ? -1 : collected->status.value_or(-1);
    if (!collected->status.has_value() && !result.timed_out) {
      result.stderr_text += "\nthe connection to the host was lost";
    }
  }
  client->Release(spawned.handle);
  return result;
}

bool RemoteProcessLauncher::StartAsync(platform::AsyncSubprocess& process,
                                       const std::vector<std::string>& argv,
                                       const std::filesystem::path& cwd,
                                       const platform::SubprocessSandbox& sandbox) const {
  // The sandbox is the local one's business (Landlock on THIS machine); a host
  // process is confined by the host account it runs as.
  (void)sandbox;
#if defined(__unix__) || defined(__APPLE__)
  const std::shared_ptr<RemoteServerClient> client = connection_->client();
  if (client == nullptr || !client->connected()) {
    return false;
  }
  auto remote = std::make_shared<RemoteAsyncProcess>(client);
  const std::optional<std::pair<int, int>> consumer = remote->MakePipes();
  if (!consumer.has_value()) {
    return false;
  }
  const RemoteServerClient::Spawned spawned =
      client->Spawn(argv, ResolveWorkingDirectory(cwd), {}, /*keep_on_detach=*/true, remote->Events());
  if (spawned.handle == 0) {
    ::close(consumer->first);
    ::close(consumer->second);
    return false;
  }
  Record(argv);
  remote->Begin(spawned.handle, spawned.pid);
  connection_->Track(remote);
  return process.Adopt(consumer->first, consumer->second, remote);
#else
  (void)process;
  (void)argv;
  (void)cwd;
  return false;
#endif
}

std::optional<RemoteServerClient::GitMetadata> RemoteProcessLauncher::Metadata(
    const std::filesystem::path& root) const {
  const std::optional<std::filesystem::path> host_root = map_.ToHost(root);
  if (!host_root.has_value()) {
    return std::nullopt;
  }
  {
    std::lock_guard lock(mutex_);
    if (const auto cached = metadata_.find(*host_root); cached != metadata_.end()) {
      return cached->second;
    }
  }
  const std::shared_ptr<RemoteServerClient> client = connection_->client();
  if (client == nullptr || !client->connected()) {
    return std::nullopt;
  }
  std::optional<RemoteServerClient::GitMetadata> answer = client->QueryGitMetadata(*host_root);
  if (answer.has_value()) {
    std::lock_guard lock(mutex_);
    metadata_[*host_root] = *answer;
  }
  return answer;
}

project::GitAvailability RemoteProcessLauncher::Availability(const std::filesystem::path& root) const {
  const auto metadata = Metadata(root);
  if (!metadata.has_value()) {
    return project::GitAvailability::Unknown;
  }
  return metadata->repository ? project::GitAvailability::Repository
                              : project::GitAvailability::NotARepository;
}

std::optional<std::filesystem::path> RemoteProcessLauncher::ReadableGitDirectory(
    const std::filesystem::path& root) const {
  if (!options_.host_paths_readable_locally) {
    return std::nullopt;
  }
  const auto metadata = Metadata(root);
  if (!metadata.has_value() || metadata->git_dir.empty()) {
    return std::nullopt;
  }
  return std::filesystem::path(metadata->git_dir);
}

void RemoteProcessLauncher::Record(const std::vector<std::string>& argv) const {
  constexpr std::size_t kRecent = 64;
  std::string line;
  for (const std::string& word : argv) {
    if (!line.empty()) {
      line += ' ';
    }
    line += word;
  }
  std::lock_guard lock(mutex_);
  ++spawns_;
  if (recent_.size() == kRecent) {
    recent_.erase(recent_.begin());
  }
  recent_.push_back(std::move(line));
}

std::vector<std::string> RemoteProcessLauncher::recent_spawns() const {
  std::lock_guard lock(mutex_);
  return recent_;
}

std::size_t RemoteProcessLauncher::spawn_count() const {
  std::lock_guard lock(mutex_);
  return spawns_;
}

}  // namespace microide::project::remote
