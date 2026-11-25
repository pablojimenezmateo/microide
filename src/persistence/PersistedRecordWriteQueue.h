#pragma once

#include <cstddef>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "util/SerialWorkQueue.h"

namespace microide::persistence {

class PersistedRecordWriteQueue {
 public:
  PersistedRecordWriteQueue();
  ~PersistedRecordWriteQueue();

  PersistedRecordWriteQueue(const PersistedRecordWriteQueue&) = delete;
  PersistedRecordWriteQueue& operator=(const PersistedRecordWriteQueue&) = delete;

  void Queue(const std::filesystem::path& path, std::vector<std::byte> body,
             std::uint32_t capability_flags);

  void QueueEncoded(const std::filesystem::path& path, std::function<bool(std::vector<std::byte>*)> encode,
                    std::uint32_t capability_flags);

  void Flush();

  void NoteOnDiskBody(const std::filesystem::path& path, const std::vector<std::byte>& body);

  void QueueDelete(const std::filesystem::path& path);

  std::size_t applied_write_count() const;

  struct WriteFailure {
    std::filesystem::path path;
    std::string reason;
  };
  std::optional<WriteFailure> CurrentFailure() const;
  std::uint64_t failure_revision() const { return failure_revision_.load(std::memory_order_acquire); }

  bool settled() const {
    return completed_sequence_.load(std::memory_order_acquire) == submitted_sequence_.load(std::memory_order_acquire);
  }

 private:
  static constexpr std::size_t kMaxMemoizedBodyBytes = 1024ull * 1024;

  void ApplyWrite(const std::filesystem::path& path, const std::string& key, std::vector<std::byte> body,
                  std::uint32_t capability_flags);

  mutable std::mutex memo_mutex_;
  std::unordered_map<std::string, std::vector<std::byte>> written_body_;
  std::size_t applied_writes_ = 0;
  struct FailedWrite {
    WriteFailure failure;
    std::vector<std::byte> body;
    std::uint32_t capability_flags = 0;
  };
  std::map<std::string, FailedWrite> failures_;
  std::atomic<std::uint64_t> failure_revision_{0};

  void SetFailureLocked(const std::string& key, std::optional<FailedWrite> failure);
  void RetryFailedWrites();

  std::uint64_t NextSequence() { return submitted_sequence_.fetch_add(1, std::memory_order_acq_rel) + 1; }
  void Finish(std::uint64_t sequence) {
    std::uint64_t seen = completed_sequence_.load(std::memory_order_acquire);
    while (seen < sequence &&
           !completed_sequence_.compare_exchange_weak(seen, sequence, std::memory_order_acq_rel)) {
    }
  }
  std::atomic<std::uint64_t> submitted_sequence_{0};
  std::atomic<std::uint64_t> completed_sequence_{0};

  util::SerialWorkQueue queue_;
};

}
