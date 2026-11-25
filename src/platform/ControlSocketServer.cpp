#include "platform/ControlSocketServer.h"
#include "platform/UnixSocket.h"

#include "platform/ControlSocketLimits.h"

#include "platform/RuntimePaths.h"

#include "util/Waker.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#include "util/Log.h"
#include "util/PosixFd.h"
#include "util/PosixPipe.h"
#include "util/ThreadName.h"

namespace microide::platform {

ControlRequestLineScan ScanControlRequestLines(std::string_view buffer,
                                               std::size_t max_line_bytes) {
  ControlRequestLineScan scan;
  std::size_t start = 0;
  while (true) {
    const std::size_t newline = buffer.find('\n', start);
    if (newline == std::string_view::npos) {
      break;
    }
    if (newline - start > max_line_bytes) {
      scan.oversize_line = true;
      break;
    }
    std::string_view line = buffer.substr(start, newline - start);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    start = newline + 1;
    scan.consumed_bytes = start;
    if (!line.empty()) {
      scan.lines.emplace_back(line);
    }
  }
  return scan;
}

namespace {

constexpr int kLingerGraceMs = 2000;

constexpr std::size_t kMaxRequestLineBytes = kMaxControlRequestLineBytes;

constexpr std::size_t kMaxInboundQueued = 4096;

constexpr std::size_t kMaxInboundQueuedBytes = 16u << 20;

constexpr std::size_t kMaxConnections = 128;

constexpr std::size_t kWriteCompactThreshold = 64u << 10;

using util::SetNonBlocking;

}

struct ControlSocketServer::Impl {
  std::string last_refused_long_path;

  struct Connection {
    int fd = -1;
    std::uint64_t id = 0;
    std::string read_buf;
    std::mutex out_mutex;
    std::string write_buf;
    std::size_t write_offset = 0;

    std::atomic<int> in_flight{0};
    bool read_closed = false;
    std::atomic<bool> write_overflow{false};
    std::chrono::steady_clock::time_point linger_deadline{};
  };

  std::filesystem::path socket_path;
  int listen_fd = -1;
  int wake_pipe[2] = {-1, -1};
  std::thread io_thread;
  std::atomic<bool> stop{false};
  std::atomic<bool> running{false};
  std::atomic<bool> rebound{false};
  std::atomic<util::WakeChannel> wake_channel{0};

  std::mutex conn_mutex;
  std::unordered_map<std::uint64_t, std::shared_ptr<Connection>> connections;
  std::uint64_t next_conn_id = 1;

  std::mutex inbound_mutex;
  std::vector<ControlInboundMessage> inbound;
  std::size_t inbound_bytes = 0;

  void PushWakeEvent() {
    util::PushWake(wake_channel.load(std::memory_order_acquire));
  }

  void WakeIoThread() {
    if (wake_pipe[1] < 0) {
      return;
    }
    const char byte = 1;
    ssize_t written = ::write(wake_pipe[1], &byte, 1);
    (void)written;
  }

  void DrainWakePipe() {
    char scratch[64];
    while (::read(wake_pipe[0], scratch, sizeof(scratch)) > 0) {
    }
  }

  bool IngestReadBuffer(Connection& conn) {
    ControlRequestLineScan scan =
        ScanControlRequestLines(conn.read_buf, kMaxRequestLineBytes);
    bool produced = false;
    bool overflow = scan.oversize_line;
    if (!scan.lines.empty()) {
      std::lock_guard<std::mutex> lock(inbound_mutex);
      for (std::string& line : scan.lines) {
        if (inbound.size() >= kMaxInboundQueued ||
            inbound_bytes + line.size() > kMaxInboundQueuedBytes) {
          overflow = true;
          break;
        }
        conn.in_flight.fetch_add(1, std::memory_order_release);
        inbound_bytes += line.size();
        inbound.push_back(ControlInboundMessage{conn.id, std::move(line)});
        produced = true;
      }
    }
    if (scan.consumed_bytes > 0) {
      conn.read_buf.erase(0, scan.consumed_bytes);
    }
    if (produced) {
      PushWakeEvent();
    }
    return !overflow;
  }

  static void CompactWriteBuffer(Connection& conn) {
    if (conn.write_offset == 0) {
      return;
    }
    if (conn.write_offset >= conn.write_buf.size()) {
      conn.write_buf.clear();
      conn.write_offset = 0;
      return;
    }
    if (conn.write_offset >= kWriteCompactThreshold ||
        conn.write_offset * 2 >= conn.write_buf.size()) {
      conn.write_buf.erase(0, conn.write_offset);
      conn.write_offset = 0;
    }
  }

  bool FlushConnection(Connection& conn) {
    std::lock_guard<std::mutex> lock(conn.out_mutex);
    while (conn.write_offset < conn.write_buf.size()) {
      const ssize_t written =
          ::send(conn.fd, conn.write_buf.data() + conn.write_offset,
                 conn.write_buf.size() - conn.write_offset, MSG_NOSIGNAL);
      if (written > 0) {
        conn.write_offset += static_cast<std::size_t>(written);
        continue;
      }
      if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        CompactWriteBuffer(conn);
        return true;
      }
      return false;
    }
    conn.write_buf.clear();
    conn.write_offset = 0;
    return true;
  }

  void IoMain() {
    while (!stop.load(std::memory_order_acquire)) {
      std::vector<std::shared_ptr<Connection>> snapshot;
      {
        std::lock_guard<std::mutex> lock(conn_mutex);
        snapshot.reserve(connections.size());
        for (auto& [id, conn] : connections) {
          snapshot.push_back(conn);
        }
      }

      std::vector<pollfd> fds;
      fds.push_back(pollfd{listen_fd, POLLIN, 0});
      fds.push_back(pollfd{wake_pipe[0], POLLIN, 0});
      for (const std::shared_ptr<Connection>& conn : snapshot) {
        short events = POLLIN;
        {
          std::lock_guard<std::mutex> lock(conn->out_mutex);
          if (!conn->write_buf.empty()) {
            events |= POLLOUT;
          }
        }
        fds.push_back(pollfd{conn->fd, events, 0});
      }

      const int ready = ::poll(fds.data(), fds.size(), 1000);
      if (stop.load(std::memory_order_acquire)) {
        break;
      }
      if (ready < 0) {
        continue;
      }
      if (ready == 0) {
        MaybeRebindSocket();
      }

      if ((fds[1].revents & POLLIN) != 0) {
        DrainWakePipe();
      }

      if ((fds[0].revents & POLLIN) != 0) {
        AcceptPending();
      }

      for (std::size_t i = 0; i < snapshot.size(); ++i) {
        const short revents = fds[i + 2].revents;
        const std::shared_ptr<Connection>& conn = snapshot[i];
        const char* drop_reason =
            (revents & (POLLERR | POLLNVAL)) != 0 ? "socket error" : nullptr;
        if (drop_reason == nullptr && (revents & POLLIN) != 0) {
          char buffer[4096];
          while (true) {
            const ssize_t count = ::recv(conn->fd, buffer, sizeof(buffer), 0);
            if (count > 0) {
              conn->read_buf.append(buffer, static_cast<std::size_t>(count));
              continue;
            }
            if (count == 0) {
              conn->read_closed = true;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
              drop_reason = "read error";
            }
            break;
          }
          if (!IngestReadBuffer(*conn)) {
            drop_reason = "request over the size cap or inbound queue full";
          }
          if (conn->read_buf.size() > kMaxRequestLineBytes) {
            drop_reason = "unterminated request over the size cap";
          }
        }
        if ((revents & POLLHUP) != 0 && conn->read_buf.empty()) {
          conn->read_closed = true;
        }
        if (drop_reason == nullptr && conn->write_overflow.load(std::memory_order_acquire)) {
          drop_reason = "client not reading: replies and events over the write buffer cap";
        }
        if (drop_reason == nullptr && !FlushConnection(*conn)) {
          drop_reason = "write error";
        }
        if (drop_reason != nullptr) {
          util::Log("control channel: dropped client " + std::to_string(conn->id) + ": " +
                    drop_reason);
          RemoveConnection(conn->id);
          continue;
        }
        if (conn->read_closed) {
          if (conn->linger_deadline == std::chrono::steady_clock::time_point{}) {
            conn->linger_deadline = std::chrono::steady_clock::now() +
                                    std::chrono::milliseconds(kLingerGraceMs);
          }
          const bool no_in_flight = conn->in_flight.load(std::memory_order_acquire) <= 0;
          bool write_pending = false;
          {
            std::lock_guard<std::mutex> lock(conn->out_mutex);
            write_pending = !conn->write_buf.empty();
          }
          const bool drained = no_in_flight && !write_pending;
          const bool expired = std::chrono::steady_clock::now() >= conn->linger_deadline;
          if (drained || expired) {
            RemoveConnection(conn->id);
          }
        }
      }
    }
  }

  void MaybeRebindSocket() {
    if (!running.load(std::memory_order_acquire) || socket_path.empty()) {
      return;
    }
    const std::string path_string = socket_path.string();
    struct stat st {};
    if (::stat(path_string.c_str(), &st) == 0) {
      return;
    }
    if (path_string.size() + 1 > sizeof(sockaddr_un::sun_path)) {
      return;
    }
    if (!EnsureSecurePrivateDirectory(socket_path.parent_path())) {
      return;
    }
    ::unlink(path_string.c_str());

    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
      return;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path_string.c_str(), path_string.size() + 1);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
      ::close(fd);
      return;
    }
    ::chmod(path_string.c_str(), S_IRUSR | S_IWUSR);
    if (::listen(fd, 8) != 0) {
      ::close(fd);
      ::unlink(path_string.c_str());
      return;
    }
    SetNonBlocking(fd);
    if (listen_fd >= 0) {
      ::close(listen_fd);
    }
    listen_fd = fd;
    rebound.store(true, std::memory_order_release);
    PushWakeEvent();
  }

  void AcceptPending() {
    while (true) {
      const int client_fd = ::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
      if (client_fd < 0) {
        break;
      }
      SetNonBlocking(client_fd);
      auto conn = std::make_shared<Connection>();
      conn->fd = client_fd;
      {
        std::lock_guard<std::mutex> lock(conn_mutex);
        if (connections.size() >= kMaxConnections) {
          ::close(client_fd);
          continue;
        }
        conn->id = next_conn_id++;
        connections.emplace(conn->id, conn);
      }
    }
  }

  void RemoveConnection(std::uint64_t id) {
    std::shared_ptr<Connection> conn;
    {
      std::lock_guard<std::mutex> lock(conn_mutex);
      auto it = connections.find(id);
      if (it == connections.end()) {
        return;
      }
      conn = it->second;
      connections.erase(it);
    }
    if (conn->fd >= 0) {
      ::close(conn->fd);
      conn->fd = -1;
    }
  }
};

ControlSocketServer::ControlSocketServer() : impl_(std::make_unique<Impl>()) {}

ControlSocketServer::~ControlSocketServer() { Stop(); }

bool ControlSocketServer::Start(const std::filesystem::path& socket_path) {
  if (impl_->running.load(std::memory_order_acquire)) {
    return false;
  }
  if (socket_path.empty()) {
    return false;
  }

  const std::string path_string = socket_path.string();
  if (path_string.size() + 1 > sizeof(sockaddr_un::sun_path)) {
    if (impl_->last_refused_long_path != path_string) {
      impl_->last_refused_long_path = path_string;
      util::Log("control channel: socket path is " + std::to_string(path_string.size()) +
                " bytes, over the " + std::to_string(sizeof(sockaddr_un::sun_path) - 1) +
                "-byte AF_UNIX limit; the channel will not start. Shorten "
                "$XDG_RUNTIME_DIR or the project path: " + path_string);
    }
    return false;
  }

  std::error_code ec;
  std::filesystem::create_directories(socket_path.parent_path(), ec);
  std::string listen_error;
  const int fd = ListenUnixSocket(socket_path, &listen_error);
  if (fd < 0) {
    util::Log("control channel: " + listen_error);
    return false;
  }

  if (!util::MakeCloexecPipe(impl_->wake_pipe, true)) {
    ::close(fd);
    ::unlink(path_string.c_str());
    return false;
  }

  impl_->listen_fd = fd;
  impl_->socket_path = socket_path;
  impl_->stop.store(false, std::memory_order_release);
  impl_->running.store(true, std::memory_order_release);
  impl_->io_thread = std::thread([impl = impl_.get()]() {
    util::NameCurrentThread("control-io");
    impl->IoMain();
  });
  return true;
}

bool ControlSocketServer::IsRunning() const {
  return impl_->running.load(std::memory_order_acquire);
}

void ControlSocketServer::Stop() {
  if (!impl_->running.exchange(false, std::memory_order_acq_rel)) {
    return;
  }
  impl_->stop.store(true, std::memory_order_release);
  impl_->WakeIoThread();
  if (impl_->io_thread.joinable()) {
    impl_->io_thread.join();
  }
  {
    std::lock_guard<std::mutex> lock(impl_->conn_mutex);
    for (auto& [id, conn] : impl_->connections) {
      if (conn->fd >= 0) {
        ::close(conn->fd);
        conn->fd = -1;
      }
    }
    impl_->connections.clear();
  }
  if (impl_->listen_fd >= 0) {
    ::close(impl_->listen_fd);
    impl_->listen_fd = -1;
  }
  for (int& fd : impl_->wake_pipe) {
    if (fd >= 0) {
      ::close(fd);
      fd = -1;
    }
  }
  if (!impl_->socket_path.empty()) {
    ::unlink(impl_->socket_path.string().c_str());
    impl_->socket_path.clear();
  }
}

void ControlSocketServer::SetWakeChannel(util::WakeChannel channel) {
  impl_->wake_channel.store(channel, std::memory_order_release);
}

std::vector<ControlInboundMessage> ControlSocketServer::TakeInbound() {
  std::vector<ControlInboundMessage> taken;
  std::lock_guard<std::mutex> lock(impl_->inbound_mutex);
  taken.swap(impl_->inbound);
  impl_->inbound_bytes = 0;
  return taken;
}

void ControlSocketServer::SendLine(std::uint64_t connection_id, const std::string& line) {
  std::shared_ptr<Impl::Connection> conn;
  {
    std::lock_guard<std::mutex> lock(impl_->conn_mutex);
    auto it = impl_->connections.find(connection_id);
    if (it == impl_->connections.end()) {
      return;
    }
    conn = it->second;
  }
  {
    std::lock_guard<std::mutex> lock(conn->out_mutex);
    const std::size_t pending = conn->write_buf.size() - conn->write_offset;
    if (pending + line.size() + 1 > ControlSocketServer::kMaxWriteBufferBytes) {
      conn->write_overflow.store(true, std::memory_order_release);
    } else {
      Impl::CompactWriteBuffer(*conn);
      conn->write_buf.append(line);
      conn->write_buf.push_back('\n');
    }
  }
  conn->in_flight.fetch_sub(1, std::memory_order_release);
  impl_->WakeIoThread();
}

void ControlSocketServer::Broadcast(const std::string& line) {
  std::vector<std::shared_ptr<Impl::Connection>> snapshot;
  {
    std::lock_guard<std::mutex> lock(impl_->conn_mutex);
    snapshot.reserve(impl_->connections.size());
    for (auto& [id, conn] : impl_->connections) {
      snapshot.push_back(conn);
    }
  }
  for (const std::shared_ptr<Impl::Connection>& conn : snapshot) {
    std::lock_guard<std::mutex> lock(conn->out_mutex);
    const std::size_t pending = conn->write_buf.size() - conn->write_offset;
    if (pending + line.size() + 1 > ControlSocketServer::kMaxWriteBufferBytes) {
      conn->write_overflow.store(true, std::memory_order_release);
      continue;
    }
    Impl::CompactWriteBuffer(*conn);
    conn->write_buf.append(line);
    conn->write_buf.push_back('\n');
  }
  if (!snapshot.empty()) {
    impl_->WakeIoThread();
  }
}

std::size_t ControlSocketServer::ConnectionCount() const {
  std::lock_guard<std::mutex> lock(impl_->conn_mutex);
  return impl_->connections.size();
}

bool ControlSocketServer::ConsumeRebound() {
  return impl_->rebound.exchange(false, std::memory_order_acq_rel);
}

}
