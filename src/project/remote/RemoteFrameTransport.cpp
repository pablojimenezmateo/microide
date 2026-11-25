#include "project/remote/RemoteFrameTransport.h"

#include <algorithm>
#include <cerrno>
#include <utility>

#include "util/PerformanceCounters.h"
#include "util/PosixFd.h"
#include "util/ThreadName.h"

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

namespace microide::project::remote {
namespace {

std::string EncodeU64(std::uint64_t value) {
  std::string bytes;
  for (int i = 0; i < 8; ++i) {
    bytes.push_back(static_cast<char>((value >> (8 * i)) & 0xffu));
  }
  return bytes;
}

std::uint64_t DecodeU64(std::string_view bytes) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[i])) << (8 * i);
  }
  return value;
}

std::uint64_t MicrosecondsSinceEpoch(std::chrono::steady_clock::time_point at) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(at.time_since_epoch()).count());
}

}

RemoteFrameTransport::~RemoteFrameTransport() {
  Stop();
}

bool RemoteFrameTransport::Start(int read_fd, int write_fd, Callbacks callbacks,
                                 Options options) {
  if (io_thread_.joinable() || read_fd < 0 || write_fd < 0) {
    return false;
  }
  if (!util::SetNonBlocking(read_fd) || (write_fd != read_fd && !util::SetNonBlocking(write_fd))) {
    return false;
  }
  wake_.Open();
  if (wake_.read_fd() < 0) {
    return false;
  }
  read_fd_ = read_fd;
  write_fd_ = write_fd;
  callbacks_ = std::move(callbacks);
  options_ = options;
  bulk_window_.store(std::clamp(options_.bulk_window_initial, options_.bulk_window_floor,
                                options_.bulk_window_cap),
                     std::memory_order_relaxed);
  io_thread_ = std::thread([this]() {
    util::NameCurrentThread("remote-io");
    Run();
  });
  return true;
}

void RemoteFrameTransport::SetFixedBulkWindow(std::size_t bytes) {
  fixed_bulk_window_.store(bytes, std::memory_order_relaxed);
  if (bytes != 0) {
    bulk_window_.store(bytes, std::memory_order_relaxed);
  }
  wake_.Wake();
}

bool RemoteFrameTransport::Send(FrameType type, Lane lane, std::uint64_t id,
                                std::string_view payload) {
  if (type == FrameType::Ack || closed()) {
    return false;
  }
  std::string encoded;
  if (!AppendFrame(encoded, type, lane, id, payload)) {
    return false;
  }
  {
    std::lock_guard lock(queue_mutex_);
    if (overflowed_ || queued_bytes_ + encoded.size() > options_.max_queued_bytes) {
      overflowed_ = true;
    } else {
      queued_bytes_ += encoded.size();
      unwritten_.fetch_add(encoded.size(), std::memory_order_acq_rel);
      Queue& queue = lane == Lane::Interactive ? interactive_ : bulk_;
      queue.frames.push_back(std::move(encoded));
      if (lane == Lane::Bulk) {
        queue.bulk_payload_bytes.push_back(payload.size());
      }
    }
  }
  wake_.Wake();
  std::lock_guard lock(queue_mutex_);
  return !overflowed_;
}

void RemoteFrameTransport::Stop() {
  stop_.store(true, std::memory_order_release);
  wake_.Wake();
  if (io_thread_.joinable()) {
    if (io_thread_.get_id() == std::this_thread::get_id()) {
      return;
    }
    io_thread_.join();
  }
  if (options_.owns_fds) {
    if (read_fd_ >= 0) {
      ::close(read_fd_);
    }
    if (write_fd_ >= 0 && write_fd_ != read_fd_) {
      ::close(write_fd_);
    }
  }
  read_fd_ = -1;
  write_fd_ = -1;
  wake_.Close();
  closed_.store(true, std::memory_order_release);
}

bool RemoteFrameTransport::Flush(std::chrono::milliseconds stall, std::chrono::milliseconds limit) {
  const auto start = std::chrono::steady_clock::now();
  auto stalled_at = start + stall;
  std::size_t lowest = unwritten_.load(std::memory_order_acquire);
  wake_.Wake();
  for (std::size_t unwritten = lowest; unwritten > 0;
       unwritten = unwritten_.load(std::memory_order_acquire)) {
    const auto now = std::chrono::steady_clock::now();
    if (unwritten < lowest) {
      lowest = unwritten;
      stalled_at = now + stall;
    }
    if (closed() || now >= stalled_at || now >= start + limit) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

void RemoteFrameTransport::Fail(std::string_view reason) {
  Close(reason);
  stop_.store(true, std::memory_order_release);
  wake_.Wake();
}

void RemoteFrameTransport::Close(std::string_view reason) {
  closed_.store(true, std::memory_order_release);
  if (!close_reported_.exchange(true) && callbacks_.on_closed) {
    callbacks_.on_closed(reason);
  }
}

bool RemoteFrameTransport::HasWritable() {
  if (writing_offset_ < writing_.size()) {
    return true;
  }
  std::lock_guard lock(queue_mutex_);
  return !interactive_.frames.empty() ||
         (!bulk_.frames.empty() &&
          bulk_bytes_sent_ - bulk_bytes_acked_ < bulk_window_.load(std::memory_order_relaxed));
}

void RemoteFrameTransport::Run() {
  std::string close_reason;
  const bool ticking = options_.tick_interval.count() > 0;
  if (ticking) {
    next_tick_ = std::chrono::steady_clock::now() + options_.tick_interval;
  }
  while (!stop_.load(std::memory_order_acquire)) {
    const bool want_write = HasWritable();
    pollfd fds[3] = {};
    nfds_t count = 0;
    fds[count++] = pollfd{read_fd_, POLLIN, 0};
    nfds_t write_index = 0;
    if (want_write) {
      if (write_fd_ == read_fd_) {
        fds[0].events |= POLLOUT;
      } else {
        write_index = count;
        fds[count++] = pollfd{write_fd_, POLLOUT, 0};
      }
    }
    const nfds_t wake_index = count;
    fds[count++] = pollfd{wake_.read_fd(), POLLIN, 0};
    int timeout_ms = -1;
    if (ticking) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= next_tick_) {
        const std::chrono::milliseconds next = callbacks_.on_tick ? callbacks_.on_tick() : options_.tick_interval;
        next_tick_ = std::chrono::steady_clock::now() + std::max(next, std::chrono::milliseconds(1));
        continue;
      }
      timeout_ms = static_cast<int>(
          std::chrono::duration_cast<std::chrono::milliseconds>(next_tick_ - now).count() + 1);
    }
    if (::poll(fds, count, timeout_ms) < 0) {
      if (errno == EINTR) {
        continue;
      }
      Close("poll failed");
      return;
    }
    if ((fds[wake_index].revents & POLLIN) != 0) {
      wake_.Drain();
    }
    if (stop_.load(std::memory_order_acquire)) {
      return;
    }
    if ((fds[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0 && !ReadAvailable()) {
      return;
    }
    {
      std::lock_guard lock(queue_mutex_);
      if (overflowed_) {
        close_reason = "the peer stopped reading (outbound queue full)";
      }
    }
    if (!close_reason.empty()) {
      Close(close_reason);
      return;
    }
    const bool write_ready =
        want_write && (write_fd_ == read_fd_ ? (fds[0].revents & POLLOUT) != 0
                                              : (fds[write_index].revents &
                                                 (POLLOUT | POLLERR | POLLHUP)) != 0);
    if ((write_ready || HasWritable()) && !FlushWrites()) {
      Close("write to the peer failed");
      return;
    }
  }
}

bool RemoteFrameTransport::FlushWrites() {
  for (;;) {
    if (writing_offset_ == writing_.size()) {
      writing_.clear();
      writing_offset_ = 0;
      std::lock_guard lock(queue_mutex_);
      if (!interactive_.frames.empty()) {
        writing_ = std::move(interactive_.frames.front());
        interactive_.frames.pop_front();
      } else if (!bulk_.frames.empty() &&
                 bulk_bytes_sent_ - bulk_bytes_acked_ <
                     bulk_window_.load(std::memory_order_relaxed)) {
        if (bulk_bytes_sent_ == bulk_bytes_acked_) {
          sample_start_.reset();
        }
        writing_ = std::move(bulk_.frames.front());
        bulk_.frames.pop_front();
        bulk_bytes_sent_ += bulk_.bulk_payload_bytes.front();
        bulk_.bulk_payload_bytes.pop_front();
        bulk_in_flight_.emplace_back(bulk_bytes_sent_, std::chrono::steady_clock::now());
      } else {
        return true;
      }
      queued_bytes_ -= std::min(queued_bytes_, writing_.size());
      util::AddPerformanceCounter(util::PerfCounterId::RemoteFramesSent);
    }
    const ssize_t written =
        ::write(write_fd_, writing_.data() + writing_offset_, writing_.size() - writing_offset_);
    if (written > 0) {
      writing_offset_ += static_cast<std::size_t>(written);
      unwritten_.fetch_sub(std::min(unwritten_.load(std::memory_order_acquire),
                                    static_cast<std::size_t>(written)),
                           std::memory_order_acq_rel);
      util::AddPerformanceCounter(util::PerfCounterId::RemoteBytesSent,
                                  static_cast<std::uint64_t>(written));
      continue;
    }
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return true;
    }
    return false;
  }
}

bool RemoteFrameTransport::ReadAvailable() {
  char buffer[64 * 1024];
  for (;;) {
    const ssize_t count = ::read(read_fd_, buffer, sizeof(buffer));
    if (count > 0) {
      last_bulk_read_ = std::chrono::steady_clock::now();
      util::AddPerformanceCounter(util::PerfCounterId::RemoteBytesReceived,
                                  static_cast<std::uint64_t>(count));
      decoder_.Feed(std::string_view(buffer, static_cast<std::size_t>(count)));
      while (std::optional<Frame> frame = decoder_.Next()) {
        util::AddPerformanceCounter(util::PerfCounterId::RemoteFramesReceived);
        if (frame->type == FrameType::Ack) {
          if (frame->lane != Lane::Interactive || frame->payload.size() != kAckPayloadBytes) {
            Close("malformed ack frame");
            return false;
          }
          HandleAck(frame->payload);
          if (closed()) {
            return false;
          }
          continue;
        }
        if (frame->lane == Lane::Bulk) {
          bulk_bytes_received_ += frame->payload.size();
          bulk_received_at_ = last_bulk_read_;
          if (bulk_bytes_received_ - bulk_bytes_received_acked_ >= options_.ack_every_bytes) {
            QueueAck();
          }
        }
        if (callbacks_.on_frame) {
          callbacks_.on_frame(std::move(*frame));
        }
        if (stop_.load(std::memory_order_acquire)) {
          return false;
        }
      }
      if (decoder_.error() != FrameDecoder::Error::None) {
        Close(FrameDecoderErrorText(decoder_.error()));
        return false;
      }
      continue;
    }
    if (count == 0) {
      Close("the peer closed the connection");
      return false;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      break;
    }
    Close("read from the peer failed");
    return false;
  }
  if (bulk_bytes_received_ > bulk_bytes_received_acked_) {
    QueueAck();
  }
  return true;
}

void RemoteFrameTransport::QueueAck() {
  std::string encoded;
  (void)AppendFrame(encoded, FrameType::Ack, Lane::Interactive, 0,
                    EncodeU64(bulk_bytes_received_) + EncodeU64(MicrosecondsSinceEpoch(bulk_received_at_)));
  bulk_bytes_received_acked_ = bulk_bytes_received_;
  std::lock_guard lock(queue_mutex_);
  queued_bytes_ += encoded.size();
  unwritten_.fetch_add(encoded.size(), std::memory_order_acq_rel);
  interactive_.frames.push_back(std::move(encoded));
}

double RemoteFrameTransport::BulkWindowTarget(double bandwidth, std::chrono::steady_clock::duration base_rtt) {
  const auto chunk = static_cast<double>(kMaxBulkChunkBytes);
  const double round_trip_bytes = bandwidth * std::chrono::duration<double>(base_rtt).count() - chunk;
  const double queue_bytes = std::max(bandwidth * std::chrono::duration<double>(kQueueAllowance).count(), chunk);
  return round_trip_bytes + queue_bytes;
}

void RemoteFrameTransport::RetargetBulkWindow() {
  const double target = base_rtt_.has_value() ? BulkWindowTarget(bulk_bandwidth_, *base_rtt_)
                                              : bulk_bandwidth_ * 0.1;
  bulk_window_.store(std::clamp(static_cast<std::size_t>(target), options_.bulk_window_floor,
                                options_.bulk_window_cap),
                     std::memory_order_relaxed);
}

void RemoteFrameTransport::HandleAck(std::string_view payload) {
  const std::uint64_t acked = DecodeU64(payload.substr(0, 8));
  const std::uint64_t received_us = DecodeU64(payload.substr(8, 8));
  if (acked > bulk_bytes_sent_ || acked < bulk_bytes_acked_) {
    Close("ack outside the bytes sent");
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  std::optional<std::chrono::steady_clock::time_point> covered_sent;
  while (!bulk_in_flight_.empty() && bulk_in_flight_.front().first <= acked) {
    covered_sent = bulk_in_flight_.front().second;
    bulk_in_flight_.pop_front();
  }
  bool base_rtt_dropped = false;
  if (covered_sent.has_value()) {
    const auto sample = now - *covered_sent;
    base_rtt_dropped = !base_rtt_.has_value() || sample < *base_rtt_;
    base_rtt_ = base_rtt_.has_value() ? std::min(*base_rtt_, sample) : sample;
    base_rtt_us_.store(std::chrono::duration_cast<std::chrono::microseconds>(*base_rtt_).count(),
                       std::memory_order_relaxed);
  }
  bulk_bytes_acked_ = acked;
  if (fixed_bulk_window_.load(std::memory_order_relaxed) != 0) {
    return;
  }
  bool sender_was_waiting = false;
  {
    std::lock_guard lock(queue_mutex_);
    sender_was_waiting = !bulk_.frames.empty();
  }
  if (const std::optional<double> sample = BandwidthSample(acked, received_us, sender_was_waiting)) {
    bulk_bandwidth_ = bulk_bandwidth_ > 0.0 ? bulk_bandwidth_ + (*sample - bulk_bandwidth_) / 8.0 : *sample;
    bulk_bandwidth_seen_.store(bulk_bandwidth_, std::memory_order_relaxed);
    RetargetBulkWindow();
  } else if (base_rtt_dropped && bulk_bandwidth_ > 0.0) {
    RetargetBulkWindow();
  }
}

std::optional<double> RemoteFrameTransport::BandwidthSample(std::uint64_t acked, std::uint64_t received_us,
                                                            bool sender_was_waiting) {
  if (!sender_was_waiting || !sample_start_.has_value() || received_us < sample_start_->second) {
    sample_start_.emplace(acked, received_us);
    return std::nullopt;
  }
  const std::uint64_t span_us = received_us - sample_start_->second;
  const std::uint64_t bytes = acked - sample_start_->first;
  constexpr auto kMinSpanUs = std::chrono::duration_cast<std::chrono::microseconds>(kMinBandwidthSpan).count();
  if (span_us < static_cast<std::uint64_t>(kMinSpanUs) || bytes < kMaxBulkChunkBytes / 2) {
    return std::nullopt;
  }
  sample_start_.emplace(acked, received_us);
  return static_cast<double>(bytes) * 1e6 / static_cast<double>(span_us);
}

}
