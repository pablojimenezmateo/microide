#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "util/MainThreadMailbox.h"
#include "util/TaskExecutor.h"
#include "util/TextFileIO.h"
#include "util/Waker.h"

namespace microide::project {

class FileReadService {
 public:
  enum class Status : std::uint8_t {
    Ok,
    Cancelled,
    Unreadable,
    TooLarge,
  };

  struct Completion {
    std::uint64_t id = 0;
    std::filesystem::path path;
    Status status = Status::Unreadable;
    std::string bytes;

    [[nodiscard]] bool ok() const { return status == Status::Ok; }
  };

  FileReadService() = default;
  FileReadService(const FileReadService&) = delete;
  FileReadService& operator=(const FileReadService&) = delete;

  ~FileReadService();

  void SetWakeChannel(util::WakeChannel channel) { mailbox_.SetWakeChannel(channel); }

  struct Request {
    std::filesystem::path path;
    std::function<void(std::string&)> on_worker;
    std::function<void(Completion)> on_complete;
    std::uintmax_t max_bytes = util::kMaxTextFileBytes;
    bool read_path = true;
  };

  std::uint64_t Begin(Request request);

  void Cancel(std::uint64_t id);

  int DrainCompletions() { return mailbox_.Drain(); }

  [[nodiscard]] bool HasCompletions() const { return mailbox_.PendingCount() > 0; }

  void FlushPendingReads();

  void CancelAllAndFlush();

  [[nodiscard]] int PendingCount() const { return pending_.load(std::memory_order_acquire); }

  [[nodiscard]] std::uint64_t PostedCount() const {
    return next_id_.load(std::memory_order_acquire);
  }

 private:
  struct InFlightRead {
    std::uint64_t id = 0;
    std::shared_ptr<std::atomic<bool>> cancelled;
  };

  std::shared_ptr<std::atomic<bool>> TrackRequest(std::uint64_t id);
  void ForgetRequest(std::uint64_t id);

  util::MainThreadMailbox mailbox_;
  std::atomic<int> pending_{0};
  std::atomic<std::uint64_t> next_id_{0};
  mutable std::mutex requests_mutex_;
  std::vector<InFlightRead> requests_;
  util::TaskExecutor executor_{1};
};

}
