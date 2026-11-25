#pragma once

#include <atomic>
#include <chrono>
#include <utility>
#include <optional>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "project/remote/RemoteFrame.h"
#include "util/WakePipe.h"

namespace microide::project::remote {

class RemoteFrameTransport {
 public:
  static constexpr std::size_t kMaxBulkChunkBytes = 64 * 1024;
  static constexpr std::chrono::milliseconds kQueueAllowance{5};

  struct Callbacks {
    std::function<void(Frame)> on_frame;
    std::function<void(std::string_view reason)> on_closed;
    std::function<std::chrono::milliseconds()> on_tick;
  };

  struct Options {
    std::size_t max_queued_bytes = 256u * 1024 * 1024;
    std::size_t bulk_window_floor = 64 * 1024;
    std::size_t bulk_window_cap = 1024 * 1024;
    std::size_t bulk_window_initial = 256 * 1024;
    std::size_t ack_every_bytes = 32 * 1024;
    bool owns_fds = true;
    std::chrono::milliseconds tick_interval{0};
  };

  RemoteFrameTransport() = default;
  ~RemoteFrameTransport();
  RemoteFrameTransport(const RemoteFrameTransport&) = delete;
  RemoteFrameTransport& operator=(const RemoteFrameTransport&) = delete;

  [[nodiscard]] bool Start(int read_fd, int write_fd, Callbacks callbacks, Options options);
  [[nodiscard]] bool Start(int read_fd, int write_fd, Callbacks callbacks) {
    return Start(read_fd, write_fd, std::move(callbacks), Options{});
  }

  bool Send(FrameType type, Lane lane, std::uint64_t id, std::string_view payload);

  bool Flush(std::chrono::milliseconds stall, std::chrono::milliseconds limit);

  void Stop();
  void Fail(std::string_view reason);

  bool closed() const { return closed_.load(std::memory_order_acquire); }
  bool OnIoThread() const { return io_thread_.get_id() == std::this_thread::get_id(); }
  std::size_t bulk_window() const { return bulk_window_.load(std::memory_order_relaxed); }
  std::chrono::microseconds base_rtt() const {
    return std::chrono::microseconds(base_rtt_us_.load(std::memory_order_relaxed));
  }
  double bulk_bandwidth() const { return bulk_bandwidth_seen_.load(std::memory_order_relaxed); }
  void SetFixedBulkWindow(std::size_t bytes);

 private:
  struct Queue {
    std::deque<std::string> frames;
    std::deque<std::size_t> bulk_payload_bytes;
  };

  void Run();
  void Close(std::string_view reason);
  bool FlushWrites();
  bool ReadAvailable();
  void HandleAck(std::string_view payload);
  void RetargetBulkWindow();
  void QueueAck();
  bool HasWritable();

  int read_fd_ = -1;
  int write_fd_ = -1;
  Callbacks callbacks_;
  Options options_;
  std::thread io_thread_;
  util::WakePipe wake_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> closed_{false};
  std::atomic<bool> close_reported_{false};
  std::atomic<std::size_t> bulk_window_{0};
  std::atomic<std::size_t> fixed_bulk_window_{0};

  std::mutex queue_mutex_;
  Queue interactive_;
  Queue bulk_;
  std::size_t queued_bytes_ = 0;
  bool overflowed_ = false;

  std::string writing_;
  std::size_t writing_offset_ = 0;
  std::atomic<std::size_t> unwritten_{0};
  FrameDecoder decoder_;
  std::uint64_t bulk_bytes_sent_ = 0;
  std::uint64_t bulk_bytes_acked_ = 0;
  std::uint64_t bulk_bytes_received_ = 0;
  std::uint64_t bulk_bytes_received_acked_ = 0;
  std::chrono::steady_clock::time_point last_ack_time_{};
  std::chrono::steady_clock::time_point next_tick_{};
  std::uint64_t last_ack_bytes_ = 0;
  std::deque<std::pair<std::uint64_t, std::chrono::steady_clock::time_point>> bulk_in_flight_;
  std::optional<std::chrono::steady_clock::duration> base_rtt_;
  double bulk_bandwidth_ = 0.0;
  std::atomic<std::int64_t> base_rtt_us_{0};
  std::atomic<double> bulk_bandwidth_seen_{0.0};
};

}
