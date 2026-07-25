#include "platform/ControlSocketServer.h"

#include "util/SdlWake.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string_view>
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

#include "util/PosixPipe.h"
#endif

#include <SDL3/SDL.h>

namespace microide::platform {

// Pure line scanner — defined outside the POSIX guard so it compiles (and is
// unit-tested) on every platform. Rejects a complete over-cap line before it is
// copied out, closing the hole where only the residual incomplete trailing line
// was bounded.
ControlRequestLineScan ScanControlRequestLines(std::string_view buffer,
                                               std::size_t max_line_bytes) {
  ControlRequestLineScan scan;
  std::size_t start = 0;
  while (true) {
    const std::size_t newline = buffer.find('\n', start);
    if (newline == std::string_view::npos) {
      break;  // only an incomplete trailing line remains; leave it unconsumed.
    }
    // Check the raw line length BEFORE materializing the substring so a hostile
    // multi-megabyte newline-terminated request is never copied into the queue.
    if (newline - start > max_line_bytes) {
      scan.oversize_line = true;
      break;  // stop before this line; caller sheds the connection.
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

#if defined(__unix__) || defined(__APPLE__)

namespace {

// Remove a stale socket at `path` in preparation for binding, but ONLY if the
// existing node is actually a socket owned by the current user. Returns false if
// the path holds a regular file, directory, or symlink (or a socket owned by
// someone else) — the caller must fail rather than blindly `unlink()` a
// user-owned file that happens to sit at the socket path. Returns true when the
// path is now clear (absent, or a stale socket we removed).
bool ClearStaleSocketPath(const std::string& path) {
  struct stat st{};
  if (::lstat(path.c_str(), &st) != 0) {
    return errno == ENOENT;  // nothing there → clear; other errors → refuse
  }
  if (!S_ISSOCK(st.st_mode) || st.st_uid != ::geteuid()) {
    return false;  // not our socket — do not delete it
  }
  return ::unlink(path.c_str()) == 0 || errno == ENOENT;
}

// How long a half-closed connection is kept alive waiting for replies to the
// requests it already sent. A client may legitimately close its write side
// immediately after sending (socat, `echo | nc`), so we must flush pending
// replies before reaping; this bounds the wait so a never-answered request can
// never leak the fd forever.
constexpr int kLingerGraceMs = 2000;

// A single control request (command or query line) is small — a few hundred
// bytes at most. This ceiling bounds the unframed read buffer so a hostile local
// client streaming bytes with no newline cannot grow it without limit (OOM). A
// peer that exceeds it is shed. 1 MiB is far above any legitimate request.
constexpr std::size_t kMaxRequestLineBytes = 1u << 20;

// Upper bound on requests accepted from all connections but not yet drained by
// the main thread. The main thread swaps the whole queue out on every control
// wake, so this only fills if a client floods complete lines faster than the
// host drains them; at the cap we shed the offending connection rather than let
// the queue grow without limit.
constexpr std::size_t kMaxInboundQueued = 4096;

// Aggregate byte budget for the shared inbound queue. The count cap above alone
// permits 4096 near-cap (~1 MiB) lines — gigabytes retained — before it trips.
// This bounds total queued bytes so a burst of large-but-legal complete lines
// cannot balloon host memory while the main thread catches up; a connection that
// would push the queue past this budget is shed like a count-cap flood.
constexpr std::size_t kMaxInboundQueuedBytes = 16u << 20;  // 16 MiB

// Bound idle connected clients. The control channel is intended for a handful of
// local tools/watchers, not hundreds of persistent sockets; without a cap, a
// local process can open idle AF_UNIX connections and grow the connection map
// and poll set without sending a byte.
constexpr std::size_t kMaxConnections = 128;

// Upper bound on a single connection's pending write buffer. A client that stops
// reading (or reads slowly) while the host keeps broadcasting — e.g. a chatty
// debug session streaming stdout via EmitEvent/Broadcast — otherwise grows
// write_buf without limit once the socket send buffer fills and every send()
// returns EAGAIN. At the cap we shed the connection, mirroring the read side.
constexpr std::size_t kMaxWriteBufferBytes = 8u << 20;

// Threshold above which a fully-consumed write-buffer prefix is reclaimed with a
// single erase during a partial-send stall, rather than left in place. Below it
// the dead prefix is small enough that skipping the memmove is the better trade.
constexpr std::size_t kWriteCompactThreshold = 64u << 10;

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
    // Index of the first not-yet-sent byte in write_buf. Partial sends advance
    // this instead of erasing from the front (which memmoves the whole pending
    // remainder on every send). The consumed prefix [0, write_offset) is
    // reclaimed lazily by CompactWriteBuffer only when it grows large or the
    // buffer fully drains, so a slow reader receiving a chatty broadcast costs
    // O(bytes) total rather than O(bytes * sends). Guarded by out_mutex.
    // INVARIANT (relied on by the POLLOUT/write-pending checks below): outside
    // FlushConnection, write_buf is non-empty iff write_offset < write_buf.size()
    // — FlushConnection clears the buffer the moment it is fully drained.
    std::size_t write_offset = 0;

    // Requests accepted from this connection that have not yet been answered.
    // Incremented on the I/O thread when a complete request line is queued
    // (IngestReadBuffer), decremented on the main thread when its reply is
    // queued (SendLine). The only field touched by both threads, hence atomic;
    // read_closed / linger_deadline are I/O-thread-only.
    std::atomic<int> in_flight{0};
    bool read_closed = false;  // peer closed its write side; flush then reap
    // Set (release) by SendLine/Broadcast when write_buf would exceed
    // kMaxWriteBufferBytes; the I/O thread observes it (acquire) and reaps the
    // connection. A slow/stalled reader must not grow host memory without bound.
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
  std::atomic<std::uint32_t> wake_event_type{0};

  std::mutex conn_mutex;
  std::unordered_map<std::uint64_t, std::shared_ptr<Connection>> connections;
  std::uint64_t next_conn_id = 1;

  std::mutex inbound_mutex;
  std::vector<ControlInboundMessage> inbound;
  // Aggregate bytes of the queued `inbound` line payloads (guarded by
  // inbound_mutex). Reset to 0 when the main thread swaps the queue out in
  // TakeInbound. Backs the kMaxInboundQueuedBytes budget.
  std::size_t inbound_bytes = 0;

  void PushWakeEvent() {
    // Checked push: inbound control messages are already queued in shared state, so a
    // rejected push must latch the shared "wake owed" bit (idle poll fallback) rather
    // than leave control-send requests waiting for an unrelated event.
    util::PushSdlWake(wake_event_type.load(std::memory_order_acquire));
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
  // Returns false when a complete line exceeds the per-request cap or the shared
  // inbound queue is saturated (count OR aggregate bytes) — the caller sheds the
  // connection; the buffer is still advanced past the lines it consumed.
  bool IngestReadBuffer(Connection& conn) {
    // Reject over-cap complete lines BEFORE copying them out (the old code
    // substr'd every complete line and only bounded the residual trailing one).
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
    // On overflow the connection is shed regardless, so discarding the whole
    // consumed prefix (rather than preserving un-queued lines) is harmless.
    if (scan.consumed_bytes > 0) {
      conn.read_buf.erase(0, scan.consumed_bytes);
    }
    if (produced) {
      PushWakeEvent();
    }
    return !overflow;
  }

  // Best-effort non-blocking flush of a connection's pending writes. Runs on the
  // I/O thread. Returns false when the peer is gone.
  // Reclaim the already-sent prefix [0, write_offset) from write_buf. Must be
  // called under conn.out_mutex. Compacts only when the consumed prefix is large
  // (absolute threshold or a majority of the buffer) so the common partial send
  // stays O(1) instead of paying an O(pending) memmove every time.
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
        CompactWriteBuffer(conn);  // reclaim consumed bytes while awaiting POLLOUT
        return true;               // retry on next POLLOUT
      }
      return false;  // hard error / closed
    }
    // Fully drained: restore the empty-iff-not-pending invariant.
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
        continue;  // EINTR or transient error; retry.
      }
      if (ready == 0) {
        // On an idle wake (poll timeout) check the advertised socket still
        // exists; some environments delete $XDG_RUNTIME_DIR contents while the
        // process lives, silently severing the query path. Re-bind if so. Fall
        // through so lingering half-closed connections are still swept (their
        // revents are 0 on a timeout, which the loop below tolerates).
        MaybeRebindSocket();
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
        bool hard_drop = (revents & (POLLERR | POLLNVAL)) != 0;
        if (!hard_drop && (revents & POLLIN) != 0) {
          char buffer[4096];
          while (true) {
            const ssize_t count = ::recv(conn->fd, buffer, sizeof(buffer), 0);
            if (count > 0) {
              conn->read_buf.append(buffer, static_cast<std::size_t>(count));
              continue;
            }
            if (count == 0) {
              conn->read_closed = true;  // peer closed its write side
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
              hard_drop = true;
            }
            break;
          }
          if (!IngestReadBuffer(*conn)) {
            // Inbound queue saturated: the peer is flooding requests faster than
            // the host can drain them. Shed it rather than grow the queue.
            hard_drop = true;
          }
          // The residual buffer holds only the incomplete trailing line (every
          // complete line was consumed above). If that single unterminated line
          // has grown past the per-request ceiling, the peer is streaming bytes
          // with no newline to exhaust memory — drop it.
          if (conn->read_buf.size() > kMaxRequestLineBytes) {
            hard_drop = true;
          }
        }
        if ((revents & POLLHUP) != 0 && conn->read_buf.empty()) {
          conn->read_closed = true;
        }
        // A stalled reader that overflowed its write buffer is shed here.
        if (!hard_drop && conn->write_overflow.load(std::memory_order_acquire)) {
          hard_drop = true;
        }
        // Always attempt to flush; send() on a non-writable socket is a cheap
        // EAGAIN. A hard write error means the peer is gone for good.
        if (!hard_drop) {
          hard_drop = !FlushConnection(*conn);
        }
        if (hard_drop) {
          RemoveConnection(conn->id);
          continue;
        }
        // Graceful lingering close: a peer that closed its write side still gets
        // replies to requests it already sent. Reap once everything is flushed
        // and answered, or when the bounded grace period expires.
        if (conn->read_closed) {
          if (conn->linger_deadline == std::chrono::steady_clock::time_point{}) {
            conn->linger_deadline = std::chrono::steady_clock::now() +
                                    std::chrono::milliseconds(kLingerGraceMs);
          }
          // Sample in_flight BEFORE write_buf. SendLine appends the reply to
          // write_buf and only THEN decrements in_flight (release), so observing
          // in_flight <= 0 here (acquire) guarantees any just-queued reply is
          // already visible to the write_buf read below. The reverse order has a
          // TOCTOU hole: read empty write_buf, SendLine appends + decrements, read
          // in_flight == 0 -> reap while a reply sits unsent (the client, which
          // half-closed its write side, then sees EOF instead of its answer).
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

  // Runs on the I/O thread (so listen_fd is mutated only here, between poll
  // iterations — no cross-thread race; Stop() touches listen_fd only after the
  // I/O thread joins). If the advertised socket file disappeared while we are
  // still running, re-bind a fresh listener at the same path and swap listen_fd.
  // Existing live connections keep their own fds and are unaffected.
  void MaybeRebindSocket() {
    if (!running.load(std::memory_order_acquire) || socket_path.empty()) {
      return;
    }
    const std::string path_string = socket_path.string();
    struct stat st {};
    if (::stat(path_string.c_str(), &st) == 0) {
      return;  // the socket file is still present
    }
    if (path_string.size() + 1 > sizeof(sockaddr_un::sun_path)) {
      return;
    }
    std::error_code ec;
    std::filesystem::create_directories(socket_path.parent_path(), ec);
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
    // Signal the main thread to re-write the discovery descriptor (it vanished
    // with the socket) on its next control drain.
    rebound.store(true, std::memory_order_release);
    PushWakeEvent();
  }

  void AcceptPending() {
    while (true) {
      // CLOEXEC on every control-channel fd (listen socket, accepted client, wake pipe):
      // without it these leak into every child forked while the channel is live (terminal
      // shells, git, debug adapters), pinning client connections open past teardown and
      // handing the child a stray control-socket handle. Matches the FileWatcher/Subprocess
      // hardening.
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
    return false;  // path too long for the address family
  }

  std::error_code ec;
  std::filesystem::create_directories(socket_path.parent_path(), ec);
  // Only clear a stale socket of our own; refuse to start (rather than delete an
  // unrelated user file) if a regular file / dir / symlink sits at the path.
  if (!ClearStaleSocketPath(path_string)) {
    return false;
  }

  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
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

  if (!util::MakeCloexecPipe(impl_->wake_pipe, /*nonblocking=*/true)) {
    ::close(fd);
    ::unlink(path_string.c_str());
    return false;
  }

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
  impl_->inbound_bytes = 0;  // the queue is now empty; reset the byte budget.
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
    if (pending + line.size() + 1 > kMaxWriteBufferBytes) {
      // Reader is stalled; shed rather than grow host memory without bound. Drop
      // this reply and flag the connection for reap on the I/O thread.
      conn->write_overflow.store(true, std::memory_order_release);
    } else {
      // Drop the already-sent prefix before appending so write_buf tracks the
      // pending bytes and does not accumulate dead prefix under steady traffic.
      Impl::CompactWriteBuffer(*conn);
      conn->write_buf.append(line);
      conn->write_buf.push_back('\n');
    }
  }
  // One reply per accepted request: balance the IngestReadBuffer increment so a
  // half-closed connection can be reaped once its replies have all been queued.
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
    if (pending + line.size() + 1 > kMaxWriteBufferBytes) {
      // Slow/stalled reader: drop the event and flag for reap rather than grow
      // host memory. A chatty debug session must not OOM the host on one client.
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
bool ControlSocketServer::ConsumeRebound() { return false; }

#endif

}  // namespace microide::platform
