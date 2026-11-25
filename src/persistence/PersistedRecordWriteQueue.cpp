#include "persistence/PersistedRecordWriteQueue.h"

#include "persistence/PersistedRecordWriter.h"
#include "util/Log.h"
#include "util/TextFileIO.h"

#include <system_error>
#include <utility>

namespace microide::persistence {
namespace {

std::string MemoKey(const std::filesystem::path& path) {
  return path.lexically_normal().string();
}

}

PersistedRecordWriteQueue::PersistedRecordWriteQueue()
    : queue_(util::SerialWorkQueue::StartMode::kLazy, {}, {}, "persist") {}

PersistedRecordWriteQueue::~PersistedRecordWriteQueue() {
  Flush();
}

void PersistedRecordWriteQueue::Queue(const std::filesystem::path& path, std::vector<std::byte> body,
                                      std::uint32_t capability_flags) {
  std::string key = MemoKey(path);
  queue_.PostLatest(key, [this, path, key, body = std::move(body), capability_flags,
                          sequence = NextSequence()]() mutable {
    ApplyWrite(path, key, std::move(body), capability_flags);
    Finish(sequence);
  });
}

void PersistedRecordWriteQueue::QueueEncoded(const std::filesystem::path& path,
                                             std::function<bool(std::vector<std::byte>*)> encode,
                                             std::uint32_t capability_flags) {
  std::string key = MemoKey(path);
  queue_.PostLatest(key, [this, path, key, encode = std::move(encode), capability_flags,
                          sequence = NextSequence()]() {
    std::vector<std::byte> body;
    if (encode && encode(&body)) {
      ApplyWrite(path, key, std::move(body), capability_flags);
    } else if (encode) {
      util::Log("persistence: could not encode " + path.string());
    }
    Finish(sequence);
  });
}

void PersistedRecordWriteQueue::ApplyWrite(const std::filesystem::path& path, const std::string& key,
                                           std::vector<std::byte> body, std::uint32_t capability_flags) {
  {
    std::lock_guard lock(memo_mutex_);
    const auto it = written_body_.find(key);
    if (it != written_body_.end() && it->second == body) {
      std::error_code error;
      if (std::filesystem::exists(path, error) && !error) {
        return;
      }
      written_body_.erase(it);
    }
  }
  PersistedRecordWriterError stage = PersistedRecordWriterError::None;
  std::error_code cause;
  if (!PersistedRecordWriter::WriteFile(path, body, capability_flags, &stage, &cause)) {
    util::Log("persistence: could not write " + path.string() + " (" +
              PersistedRecordWriterErrorName(stage) +
              (cause ? ": " + cause.message() : std::string()) + ")");
    std::lock_guard lock(memo_mutex_);
    written_body_.erase(key);
    SetFailureLocked(key, FailedWrite{.failure = WriteFailure{.path = path,
                                                              .reason = cause ? util::DescribeWriteError(cause)
                                                                              : std::string(PersistedRecordWriterErrorName(stage))},
                                      .body = std::move(body),
                                      .capability_flags = capability_flags});
    return;
  }
  {
    std::lock_guard lock(memo_mutex_);
    SetFailureLocked(key, std::nullopt);
    if (body.size() > kMaxMemoizedBodyBytes) {
      written_body_.erase(key);
    } else {
      written_body_[key] = std::move(body);
    }
    ++applied_writes_;
  }
  RetryFailedWrites();
}

void PersistedRecordWriteQueue::RetryFailedWrites() {
  std::map<std::string, FailedWrite> pending;
  {
    std::lock_guard lock(memo_mutex_);
    if (failures_.empty()) {
      return;
    }
    pending = failures_;
  }
  for (auto& [key, failed] : pending) {
    if (!PersistedRecordWriter::WriteFile(failed.failure.path, failed.body, failed.capability_flags)) {
      continue;
    }
    std::lock_guard lock(memo_mutex_);
    const auto it = failures_.find(key);
    if (it == failures_.end() || it->second.body != failed.body) {
      continue;
    }
    SetFailureLocked(key, std::nullopt);
    if (failed.body.size() <= kMaxMemoizedBodyBytes) {
      written_body_[key] = std::move(failed.body);
    }
    ++applied_writes_;
  }
}

void PersistedRecordWriteQueue::Flush() { queue_.Flush(); }

void PersistedRecordWriteQueue::NoteOnDiskBody(const std::filesystem::path& path,
                                               const std::vector<std::byte>& body) {
  std::lock_guard lock(memo_mutex_);
  const std::string key = MemoKey(path);
  if (body.size() > kMaxMemoizedBodyBytes) {
    written_body_.erase(key);
    return;
  }
  written_body_[key] = body;
}

void PersistedRecordWriteQueue::QueueDelete(const std::filesystem::path& path) {
  std::string key = MemoKey(path);
  queue_.PostLatest(key, [this, path, key, sequence = NextSequence()]() {
    {
      std::lock_guard lock(memo_mutex_);
      written_body_.erase(key);
      SetFailureLocked(key, std::nullopt);
    }
    std::error_code error;
    std::filesystem::remove(path, error);
    error.clear();
    std::filesystem::remove(PersistedRecordWriter::BackupPathFor(path), error);
    Finish(sequence);
  });
}

void PersistedRecordWriteQueue::SetFailureLocked(const std::string& key, std::optional<FailedWrite> failure) {
  const auto it = failures_.find(key);
  if (!failure.has_value()) {
    if (it != failures_.end()) {
      failures_.erase(it);
      failure_revision_.fetch_add(1, std::memory_order_acq_rel);
    }
    return;
  }
  const bool same_reason = it != failures_.end() && it->second.failure.reason == failure->failure.reason;
  failures_[key] = std::move(*failure);
  if (!same_reason) {
    failure_revision_.fetch_add(1, std::memory_order_acq_rel);
  }
}

std::optional<PersistedRecordWriteQueue::WriteFailure> PersistedRecordWriteQueue::CurrentFailure() const {
  std::lock_guard lock(memo_mutex_);
  if (failures_.empty()) {
    return std::nullopt;
  }
  return failures_.begin()->second.failure;
}

std::size_t PersistedRecordWriteQueue::applied_write_count() const {
  std::lock_guard lock(memo_mutex_);
  return applied_writes_;
}

}
