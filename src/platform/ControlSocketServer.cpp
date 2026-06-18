#include "platform/ControlSocketServer.h"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#endif

#include <SDL3/SDL.h>

namespace microide::platform {

#if defined(__unix__) || defined(__APPLE__)

namespace {

void SetNonBlocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
}

}  // namespace

struct ControlSocketServer::Impl {
  struct Connection {
    int fd = -1;
    std::uint64_t id = 0;
    std::string read_buf;
    std::mutex out_mutex;
    std::string write_buf;  // bytes pending write (already framed with '\n')
  };

  std::filesystem::path socket_path;
  int listen_fd = -1;
  int wake_pipe[2] = {-1, -1};
  std::thread io_thread;
  std::atomic<bool> stop{false};
  std::atomic<bool> running{false};
  std::atomic<std::uint32_t> wake_event_type{0};

  std::mutex conn_mutex;
  std::unordered_map<std::uint64_t, std::shared_ptr<Connection>> connections;
  std::uint64_t next_conn_id = 1;

  std::mutex inbound_mutex;
  std::vector<ControlInboundMessage> inbound;

  void PushWakeEvent() {
    const std::uint32_t event_type = wake_event_type.load(std::memory_order_acquire);
    if (event_type == 0) {
      return;
    }
    SDL_Event ev{};
    ev.type = event_type;
    SDL_PushEvent(&ev);
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

  // Split the connection's read buffer into complete lines, enqueue each as an
  // inbound message, and request a main-thread wake. Runs on the I/O thread.
  void IngestReadBuffer(Connection& conn) {
    std::size_t start = 0;
    bool produced = false;
    while (true) {
      const std::size_t newline = conn.read_buf.find('\n', start);
      if (newline == std::string::npos) {
        break;
      }
      std::string line = conn.read_buf.substr(start, newline - start);
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      start = newline + 1;
      if (!line.empty()) {
        std::lock_guard<std::mutex> lock(inbound_mutex);
        inbound.push_back(ControlInboundMessage{conn.id, std::move(line)});
        produced = true;
      }
    }
    if (start > 0) {
      conn.read_buf.erase(0, start);
    }
    if (produced) {
      PushWakeEvent();
    }
  }

  // Best-effort non-blocking flush of a connection's pending writes. Runs on the
  // I/O thread. Returns false when the peer is gone.
  bool FlushConnection(Connection& conn) {
    std::lock_guard<std::mutex> lock(conn.out_mutex);
    while (!conn.write_buf.empty()) {
      const ssize_t written =
          ::send(conn.fd, conn.write_buf.data(), conn.write_buf.size(), MSG_NOSIGNAL);
      if (written > 0) {
        conn.write_buf.erase(0, static_cast<std::size_t>(written));
        continue;
      }
      if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return true;  // retry on next POLLOUT
      }
      return false;  // hard error / closed
    }
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
      if (ready <= 0) {
        continue;
      }

      if ((fds[1].revents & POLLIN) != 0) {
        DrainWakePipe();
      }

      if ((fds[0].revents & POLLIN) != 0) {
        AcceptPending();
      }

      // Connection fds start at index 2, aligned with `snapshot`.
      for (std::size_t i = 0; i < snapshot.size(); ++i) {
        const short revents = fds[i + 2].revents;
        const std::shared_ptr<Connection>& conn = snapshot[i];
        bool drop = (revents & (POLLERR | POLLNVAL)) != 0;
        if (!drop && (revents & POLLIN) != 0) {
          char buffer[4096];
          while (true) {
            const ssize_t count = ::recv(conn->fd, buffer, sizeof(buffer), 0);
            if (count > 0) {
              conn->read_buf.append(buffer, static_cast<std::size_t>(count));
              continue;
            }
            if (count == 0) {
              drop = true;  // peer closed
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
              drop = true;
            }
            break;
          }
          IngestReadBuffer(*conn);
        }
        if (!drop && (revents & POLLOUT) != 0) {
          drop = !FlushConnection(*conn);
        }
        if (!drop && (revents & POLLHUP) != 0 && conn->read_buf.empty()) {
          drop = true;
        }
        if (drop) {
          RemoveConnection(conn->id);
        } else {
          drop = !FlushConnection(*conn);
          if (drop) {
            RemoveConnection(conn->id);
          }
        }
      }
    }
  }

  void AcceptPending() {
    while (true) {
      const int client_fd = ::accept(listen_fd, nullptr, nullptr);
      if (client_fd < 0) {
        break;
      }
      SetNonBlocking(client_fd);
      auto conn = std::make_shared<Connection>();
      conn->fd = client_fd;
      {
        std::lock_guard<std::mutex> lock(conn_mutex);
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
    return false;  // path too long for the address family
  }

  std::error_code ec;
  std::filesystem::create_directories(socket_path.parent_path(), ec);
  ::unlink(path_string.c_str());

  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return false;
  }

  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, path_string.c_str(), path_string.size() + 1);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    ::close(fd);
    return false;
  }
  ::chmod(path_string.c_str(), S_IRUSR | S_IWUSR);
  if (::listen(fd, 8) != 0) {
    ::close(fd);
    ::unlink(path_string.c_str());
    return false;
  }
  SetNonBlocking(fd);

  if (::pipe(impl_->wake_pipe) != 0) {
    ::close(fd);
    ::unlink(path_string.c_str());
    return false;
  }
  SetNonBlocking(impl_->wake_pipe[0]);
  SetNonBlocking(impl_->wake_pipe[1]);

  impl_->listen_fd = fd;
  impl_->socket_path = socket_path;
  impl_->stop.store(false, std::memory_order_release);
  impl_->running.store(true, std::memory_order_release);
  impl_->io_thread = std::thread([impl = impl_.get()]() { impl->IoMain(); });
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

void ControlSocketServer::SetWakeEventType(std::uint32_t event_type) {
  impl_->wake_event_type.store(event_type, std::memory_order_release);
}

std::vector<ControlInboundMessage> ControlSocketServer::TakeInbound() {
  std::vector<ControlInboundMessage> taken;
  std::lock_guard<std::mutex> lock(impl_->inbound_mutex);
  taken.swap(impl_->inbound);
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
    conn->write_buf.append(line);
    conn->write_buf.push_back('\n');
  }
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

#else  // non-POSIX: control channel unsupported.

struct ControlSocketServer::Impl {};
ControlSocketServer::ControlSocketServer() : impl_(nullptr) {}
ControlSocketServer::~ControlSocketServer() = default;
bool ControlSocketServer::Start(const std::filesystem::path&) { return false; }
bool ControlSocketServer::IsRunning() const { return false; }
void ControlSocketServer::Stop() {}
void ControlSocketServer::SetWakeEventType(std::uint32_t) {}
std::vector<ControlInboundMessage> ControlSocketServer::TakeInbound() { return {}; }
void ControlSocketServer::SendLine(std::uint64_t, const std::string&) {}
void ControlSocketServer::Broadcast(const std::string&) {}
std::size_t ControlSocketServer::ConnectionCount() const { return 0; }

#endif

}  // namespace microide::platform
