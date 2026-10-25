#pragma once

// A delaying link for remote tests and perf scenarios: it splices an upstream
// descriptor (a connection to microide-server) to a fresh socketpair whose other
// end the client takes, and holds every chunk for half the round trip in each
// direction, so the client sees `rtt` between a request and its answer. It can
// stall the server's direction (a server that stops answering mid-stream) or
// blackhole both (a link that dies without a FIN, which only the heartbeat can
// detect). Header-only and free of the test harness so the perf binary can use it.

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace microide::tests {

class DelayRelay {
 public:
  // Takes ownership of `upstream_fd`. `rtt` is split evenly between directions.
  // `bytes_per_second` (0 = unlimited) paces each direction like a link's
  // serialization delay, so a bulk transfer queues behind itself the way it does
  // on a real link and an interactive byte has to get through that queue.
  DelayRelay(int upstream_fd, std::chrono::milliseconds rtt, std::uint64_t bytes_per_second = 0)
      : upstream_fd_(upstream_fd), one_way_(rtt / 2), bytes_per_second_(bytes_per_second) {
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
    // Small reads: each is paced as one unit, so a large read would act like a
    // 64 KiB packet and hold an interactive byte that shared it behind the whole
    // thing. Real links serialize per ~1.5 KiB packet; 4 KiB is close enough.
    char buffer[4 * 1024];
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
    auto link_free = std::chrono::steady_clock::now();  // when the "wire" is idle again
    while (!stop_) {
      const bool stalled = d.stall != nullptr && d.stall->load();
      if (d.queue.empty() || stalled) {
        d.wake.wait_for(lock, std::chrono::milliseconds(20));
        continue;
      }
      const auto due = std::max(d.queue.front().due, link_free);
      if (std::chrono::steady_clock::now() < due) {
        d.wake.wait_until(lock, due);
        continue;
      }
      Chunk chunk = std::move(d.queue.front());
      d.queue.pop_front();
      if (bytes_per_second_ != 0) {
        // A virtual clock: this chunk left at `due` (not when the thread woke),
        // so ordinary timer oversleep does not eat into the modelled bandwidth.
        // But it may lag real time by at most one chunk: a writer that woke very
        // late (a sanitizer build, a loaded box) must not then send everything
        // that came due back to back -- a burst above the link's rate, which a
        // real link cannot produce and an ack-measured bandwidth would believe.
        const auto transmit = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(static_cast<double>(chunk.bytes.size()) /
                                          static_cast<double>(bytes_per_second_)));
        link_free = std::max(due, std::chrono::steady_clock::now() - transmit) + transmit;
      }
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
  std::uint64_t bytes_per_second_ = 0;
  std::atomic_bool stop_{false};
  std::atomic_bool stall_to_client_{false};
  std::atomic_bool blackhole_{false};
  Direction to_server_;
  Direction to_client_;
};

}  // namespace microide::tests

