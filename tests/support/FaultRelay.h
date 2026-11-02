#pragma once

// A frame-aware link between a remote client and microide-server, for fault
// injection: it splices an upstream descriptor (the daemon's socket) to a fresh
// socketpair whose other end the client takes, forwards whole wire frames
// (project/remote/RemoteFrame.h), optionally after a one-way delay, and counts
// them once armed. When the count reaches the armed step, it runs the fault --
// SIGKILL a process, raise SIGKILL on itself, or cut the link -- at that exact
// frame boundary, so a matrix can put the same fault between every pair of
// frames an operation exchanges.
//
// Ack frames are the transport's own flow control, not a step of any operation,
// and are forwarded uncounted. A cut drops the frame that triggered it and
// everything after, in both directions, and shuts both sockets: the client sees
// the link close, the daemon sees its connection end (and, like over a dead ssh
// link, carries on with whatever it had already received).

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace microide::tests {

class FaultRelay {
 public:
  // Takes ownership of `upstream_fd`.
  FaultRelay(int upstream_fd, std::chrono::milliseconds one_way) : upstream_fd_(upstream_fd), one_way_(one_way) {
    int pair[2] = {-1, -1};
    if (upstream_fd_ >= 0 && ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0) {
      client_fd_ = pair[0];
      relay_fd_ = pair[1];
    }
    to_server_.from = relay_fd_;
    to_server_.to = upstream_fd_;
    to_client_.from = upstream_fd_;
    to_client_.to = relay_fd_;
    Start(to_server_);
    Start(to_client_);
  }

  ~FaultRelay() {
    stop_ = true;
    Shut();
    for (Direction* d : {&to_server_, &to_client_}) {
      std::lock_guard lock(d->mutex);
      d->wake.notify_all();
    }
    for (Direction* d : {&to_server_, &to_client_}) {
      for (std::thread* t : {&d->reader, &d->writer}) {
        if (t->joinable()) {
          t->join();
        }
      }
    }
    if (timer_.joinable()) {
      timer_.join();
    }
    for (int fd : {upstream_fd_, relay_fd_, client_fd_}) {
      if (fd >= 0) {
        ::close(fd);
      }
    }
  }

  FaultRelay(const FaultRelay&) = delete;
  FaultRelay& operator=(const FaultRelay&) = delete;

  bool ok() const { return client_fd_ >= 0 && relay_fd_ >= 0; }
  int TakeClientFd() {
    const int fd = client_fd_;
    client_fd_ = -1;
    return fd;
  }

  // From now on, count frames; when frame number `step` (0-based) arrives, run
  // `fault` on the relay's thread before forwarding it, and with `cut` drop it
  // and close the link. With `after`, the frame is delivered first and the fault
  // runs that long after it was (the peer is part-way through handling it).
  // SIZE_MAX counts without ever firing.
  void Arm(std::size_t step, std::function<void()> fault, bool cut,
           std::optional<std::chrono::microseconds> after = std::nullopt) {
    std::lock_guard lock(count_mutex_);
    armed_ = true;
    counted_ = 0;
    step_ = step;
    fault_ = std::move(fault);
    cut_on_fault_ = cut;
    after_ = after;
    fired_ = false;
    fault_ran_ = false;
  }
  std::size_t counted() const {
    std::lock_guard lock(count_mutex_);
    return counted_;
  }
  bool fired() const {
    std::lock_guard lock(count_mutex_);
    return fired_;
  }
  // Once fired(), until the fault itself has run (a delayed one runs later): what
  // follows a fault must not start before it, or the fault lands on the recovery.
  void WaitForFault() {
    std::unique_lock lock(count_mutex_);
    fault_cv_.wait(lock, [this] { return !fired_ || fault_ran_; });
  }

 private:
  static constexpr std::size_t kHeader = 4 + 2 + 1 + 8;
  static constexpr std::uint16_t kAckType = 4;

  struct Chunk {
    std::chrono::steady_clock::time_point due;
    std::string bytes;
    bool fire_after = false;  // run the fault once this is delivered
  };
  enum class Verdict { Forward, Drop, ForwardThenFire };
  struct Direction {
    int from = -1;
    int to = -1;
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Chunk> queue;
    bool eof = false;  // the reader saw the end of stream (mutex)
    std::thread reader;
    std::thread writer;
  };

  void Start(Direction& d) {
    d.reader = std::thread([this, &d] { Read(d); });
    d.writer = std::thread([this, &d] { Write(d); });
  }

  void Shut() {
    for (int fd : {upstream_fd_, relay_fd_}) {
      if (fd >= 0) {
        ::shutdown(fd, SHUT_RDWR);
      }
    }
  }

  // Counts the frame and runs the fault on its step.
  Verdict Count(std::uint16_t type) {
    const Verdict pass = cut_ ? Verdict::Drop : Verdict::Forward;
    if (type == kAckType) {
      return pass;
    }
    std::function<void()> fault;
    bool cut = false;
    {
      std::lock_guard lock(count_mutex_);
      if (!armed_ || cut_) {
        return pass;
      }
      const std::size_t index = counted_++;
      if (index != step_ || fired_) {
        return Verdict::Forward;
      }
      fired_ = true;
      if (after_.has_value()) {
        return Verdict::ForwardThenFire;
      }
      fault = fault_;
      cut = cut_on_fault_;
      if (cut) {
        cut_ = true;
      }
    }
    if (fault) {
      fault();
    }
    MarkFaultRan();
    if (cut) {
      Shut();
      return Verdict::Drop;
    }
    return Verdict::Forward;
  }

  void MarkFaultRan() {
    std::lock_guard lock(count_mutex_);
    fault_ran_ = true;
    fault_cv_.notify_all();
  }

  // The delayed fault, once its frame is on the peer's socket.
  void FireAfterDelivery() {
    std::function<void()> fault;
    std::chrono::microseconds delay{0};
    bool cut = false;
    {
      std::lock_guard lock(count_mutex_);
      fault = fault_;
      delay = after_.value_or(std::chrono::microseconds(0));
      cut = cut_on_fault_;
    }
    timer_ = std::thread([this, fault = std::move(fault), delay, cut] {
      std::this_thread::sleep_for(delay);
      if (cut) {
        cut_ = true;
        Shut();
      }
      if (fault) {
        fault();
      }
      MarkFaultRan();
    });
  }

  void Read(Direction& d) {
    std::string pending;
    char buffer[16 * 1024];
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
        // Pass the end of stream on once what is queued has gone.
        std::lock_guard lock(d.mutex);
        d.eof = true;
        d.wake.notify_all();
        return;
      }
      pending.append(buffer, static_cast<std::size_t>(n));
      while (pending.size() >= kHeader) {
        std::uint32_t length = 0;
        std::uint16_t type = 0;
        std::memcpy(&length, pending.data(), 4);  // little-endian hosts only, like the tests
        std::memcpy(&type, pending.data() + 4, 2);
        const std::size_t total = kHeader + length;
        if (pending.size() < total) {
          break;
        }
        std::string frame = pending.substr(0, total);
        pending.erase(0, total);
        const Verdict verdict = Count(type);
        if (verdict == Verdict::Drop) {
          continue;
        }
        std::lock_guard lock(d.mutex);
        d.queue.push_back(Chunk{std::chrono::steady_clock::now() + one_way_, std::move(frame),
                                verdict == Verdict::ForwardThenFire});
        d.wake.notify_all();
      }
    }
  }

  void Write(Direction& d) {
    std::unique_lock lock(d.mutex);
    while (!stop_) {
      if (d.queue.empty()) {
        if (d.eof) {
          ::shutdown(d.to, SHUT_WR);
          return;
        }
        d.wake.wait_for(lock, std::chrono::milliseconds(20));
        continue;
      }
      if (std::chrono::steady_clock::now() < d.queue.front().due) {
        d.wake.wait_until(lock, d.queue.front().due);
        continue;
      }
      Chunk chunk = std::move(d.queue.front());
      d.queue.pop_front();
      lock.unlock();
      if (!cut_) {
        std::size_t sent = 0;
        while (sent < chunk.bytes.size() && !stop_) {
          const ssize_t n = ::send(d.to, chunk.bytes.data() + sent, chunk.bytes.size() - sent, MSG_NOSIGNAL);
          if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) {
              continue;
            }
            break;
          }
          sent += static_cast<std::size_t>(n);
        }
      }
      if (chunk.fire_after) {
        FireAfterDelivery();
      }
      lock.lock();
    }
  }

  int upstream_fd_ = -1;
  int relay_fd_ = -1;
  int client_fd_ = -1;
  std::chrono::steady_clock::duration one_way_;
  std::atomic_bool stop_{false};
  std::atomic_bool cut_{false};

  mutable std::mutex count_mutex_;
  bool armed_ = false;
  std::size_t counted_ = 0;
  std::size_t step_ = std::numeric_limits<std::size_t>::max();
  std::function<void()> fault_;
  bool cut_on_fault_ = false;
  bool fired_ = false;
  bool fault_ran_ = false;
  std::condition_variable fault_cv_;
  std::optional<std::chrono::microseconds> after_;
  std::thread timer_;  // the delayed fault (one per arming)

  Direction to_server_;
  Direction to_client_;
};

}  // namespace microide::tests
