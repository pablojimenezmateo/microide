#pragma once

// A delaying link for remote tests and perf scenarios: it splices an upstream
// descriptor (a connection to microide-server) to a fresh socketpair whose other
// end the client takes, and holds every chunk for half the round trip in each
// direction, so the client sees `rtt` between a request and its answer. It can
// stall the server's direction (a server that stops answering mid-stream) or
// blackhole both (a link that dies without a FIN, which only the heartbeat can
// detect). Header-only and free of the test harness so the perf binary can use it.

#if defined(__unix__) || defined(__APPLE__)

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace microide::tests {

class DelayRelay {
 public:
  // Takes ownership of `upstream_fd`. `rtt` is split evenly between directions.
  DelayRelay(int upstream_fd, std::chrono::milliseconds rtt)
      : upstream_fd_(upstream_fd), one_way_(rtt / 2) {
    int pair[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0) {
      client_fd_ = pair[0];
      relay_fd_ = pair[1];
    }
    to_server_.from = relay_fd_;
    to_server_.to = upstream_fd_;
    to_client_.from = upstream_fd_;
    to_client_.to = relay_fd_;
    to_client_.stall = &stall_to_client_;
    Start(to_server_);
    Start(to_client_);
  }

  ~DelayRelay() {
    stop_ = true;
    // Unblock a writer stuck on a full socket before joining it.
    for (int fd : {upstream_fd_, relay_fd_}) {
      if (fd >= 0) {
        ::shutdown(fd, SHUT_RDWR);
      }
    }
    for (Direction* d : {&to_server_, &to_client_}) {
      {
        std::lock_guard lock(d->mutex);
        d->wake.notify_all();
      }
    }
    for (Direction* d : {&to_server_, &to_client_}) {
      for (std::thread* t : {&d->reader, &d->writer}) {
        if (t->joinable()) {
          t->join();
        }
      }
    }
    for (int fd : {upstream_fd_, relay_fd_, client_fd_}) {
      if (fd >= 0) {
        ::close(fd);
      }
    }
  }

  DelayRelay(const DelayRelay&) = delete;
  DelayRelay& operator=(const DelayRelay&) = delete;

  bool ok() const { return client_fd_ >= 0 && relay_fd_ >= 0; }
  // The client's end; the caller owns it from here (ConnectFds takes it).
  int TakeClientFd() {
    const int fd = client_fd_;
    client_fd_ = -1;
    return fd;
  }
  // Hold the server's bytes (a stalled server); released when cleared.
  void SetServerStalled(bool stalled) {
    stall_to_client_ = stalled;
    std::lock_guard lock(to_client_.mutex);
    to_client_.wake.notify_all();
  }
  // Drop everything in both directions from now on, with no FIN: a dead link.
  void Blackhole() { blackhole_ = true; }

 private:
  struct Chunk {
    std::chrono::steady_clock::time_point due;
    std::string bytes;
  };
  struct Direction {
    int from = -1;
    int to = -1;
    const std::atomic_bool* stall = nullptr;
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Chunk> queue;
    std::thread reader;
    std::thread writer;
  };

  void Start(Direction& d) {
    d.reader = std::thread([this, &d] { Read(d); });
    d.writer = std::thread([this, &d] { Write(d); });
  }

  void Read(Direction& d) {
    char buffer[64 * 1024];
    while (!stop_) {
      pollfd pfd{d.from, POLLIN, 0};
      const int ready = ::poll(&pfd, 1, 20);
      if (ready <= 0) {
        continue;
      }
      const ssize_t n = ::read(d.from, buffer, sizeof(buffer));
      if (n <= 0) {
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
          continue;
        }
        ::shutdown(d.to, SHUT_WR);  // pass the end of stream on
        return;
      }
      if (blackhole_) {
        continue;
      }
      std::lock_guard lock(d.mutex);
      d.queue.push_back(Chunk{std::chrono::steady_clock::now() + one_way_,
                              std::string(buffer, static_cast<std::size_t>(n))});
      d.wake.notify_all();
    }
  }

  void Write(Direction& d) {
    std::unique_lock lock(d.mutex);
    while (!stop_) {
      const bool stalled = d.stall != nullptr && d.stall->load();
      if (d.queue.empty() || stalled) {
        d.wake.wait_for(lock, std::chrono::milliseconds(20));
        continue;
      }
      const auto due = d.queue.front().due;
      if (std::chrono::steady_clock::now() < due) {
        d.wake.wait_until(lock, due);
        continue;
      }
      Chunk chunk = std::move(d.queue.front());
      d.queue.pop_front();
      lock.unlock();
      if (!blackhole_) {
        std::size_t sent = 0;
        while (sent < chunk.bytes.size() && !stop_) {
          const ssize_t n =
              ::send(d.to, chunk.bytes.data() + sent, chunk.bytes.size() - sent, MSG_NOSIGNAL);
          if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) {
              continue;
            }
            break;
          }
          sent += static_cast<std::size_t>(n);
        }
      }
      lock.lock();
    }
  }

  int upstream_fd_ = -1;
  int relay_fd_ = -1;
  int client_fd_ = -1;
  std::chrono::steady_clock::duration one_way_;
  std::atomic_bool stop_{false};
  std::atomic_bool stall_to_client_{false};
  std::atomic_bool blackhole_{false};
  Direction to_server_;
  Direction to_client_;
};

}  // namespace microide::tests

#endif
