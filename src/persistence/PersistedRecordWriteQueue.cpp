#include "persistence/PersistedRecordWriteQueue.h"

#include "persistence/PersistedRecordWriter.h"
#include "util/Log.h"

#include <system_error>
#include <utility>

namespace microide::persistence {
namespace {

// Same normalization the recovery guard uses, so one path spelled two ways is
// one memo entry and one coalescing key. Purely lexical on purpose: it runs on
// the worker for every write and must not touch the filesystem.
std::string MemoKey(const std::filesystem::path& path) {
  return path.lexically_normal().string();
}

}  // namespace

PersistedRecordWriteQueue::PersistedRecordWriteQueue()
    : queue_(util::SerialWorkQueue::StartMode::kLazy, {}, {}, "persist") {}

PersistedRecordWriteQueue::~PersistedRecordWriteQueue() {
  // Flush before the worker is torn down: whatever is still queued is state the
  // user produced and that exists nowhere else yet. SerialWorkQueue's destructor
  // Shutdown()s, which CANCELS the backlog -- relying on that would silently drop
  // the last save of every session.
  Flush();
}

void PersistedRecordWriteQueue::Queue(const std::filesystem::path& path, std::vector<std::byte> body,
                                      std::uint32_t capability_flags) {
  std::string key = MemoKey(path);
  // Coalesce on the path: a burst of saves for the same record collapses to the
  // last one, which is the only one whose bytes matter.
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
      // Already on disk byte for byte. The existence check keeps that honest:
      // if the file went away underneath us the memo is stale and the record
      // has to be rewritten.
      std::error_code error;
      if (std::filesystem::exists(path, error) && !error) {
        return;
      }
      written_body_.erase(it);
    }
  }
  if (!PersistedRecordWriter::WriteFile(path, body, capability_flags)) {
    // Said, because nothing else does: the caller was told "accepted" long ago,
    // and a session that silently stops landing is a kill -9 that restores an
    // older one (soak 55673209c35c and kin had no other trace to go on).
    util::Log("persistence: could not write " + path.string());
    // Leave the memo without an entry for this path so the next save retries
    // rather than treating the failed write as persisted.
    std::lock_guard lock(memo_mutex_);
    written_body_.erase(key);
    return;
  }
  std::lock_guard lock(memo_mutex_);
  if (body.size() > kMaxMemoizedBodyBytes) {
    written_body_.erase(key);
  } else {
    written_body_[key] = std::move(body);
  }
  ++applied_writes_;
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
      // The record is going away, so the memo of what it held must go with it, or
      // a later save of that same body would be skipped against a file that no
      // longer exists.
      std::lock_guard lock(memo_mutex_);
      written_body_.erase(key);
    }
    std::error_code error;
    std::filesystem::remove(path, error);
    // Also remove the backup, or the reader would fall back to it and resurrect
    // the state this call is meant to clear.
    error.clear();
    std::filesystem::remove(PersistedRecordWriter::BackupPathFor(path), error);
    Finish(sequence);
  });
}

std::size_t PersistedRecordWriteQueue::applied_write_count() const {
  std::lock_guard lock(memo_mutex_);
  return applied_writes_;
}

}  // namespace microide::persistence
