#pragma once

#include <cstddef>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "util/SerialWorkQueue.h"

namespace microide::persistence {

// Applies persisted-record writes on a background thread instead of the shell
// thread.
//
// A record write is temp-write + fsync + backup rotate + atomic rename, and the
// fsync dominates it: ~1 ms per call, every one of them on the shell thread, and
// workspace-session state is rewritten constantly (30 of 41 record writes in one
// smoke scenario). That made saving UI state the largest single main-thread scope
// in the perf suite -- a stall the user sees on project switch and on close.
//
// Nothing about the write itself changes: the fsync still happens, still BEFORE
// the rename, so the crash-durability ordering is exactly what it was. Only the
// thread changes. What callers give up is the synchronous result: Queue() reports
// that the record was accepted, not that it has landed. Flush() is how a caller
// that needs the bytes on disk right now -- shutdown, or reading the file back --
// gets the old guarantee.
//
// Writes are serialized on one worker, so they land in submission order. Two
// queues for the same path coalesce to the latest body: a burst of session saves
// costs one write, not one per save.
class PersistedRecordWriteQueue {
 public:
  PersistedRecordWriteQueue();
  ~PersistedRecordWriteQueue();

  PersistedRecordWriteQueue(const PersistedRecordWriteQueue&) = delete;
  PersistedRecordWriteQueue& operator=(const PersistedRecordWriteQueue&) = delete;

  // Accept `body` for `path`. Returns immediately.
  void Queue(const std::filesystem::path& path, std::vector<std::byte> body,
             std::uint32_t capability_flags);

  // The same, with the body ENCODED on the worker: `encode` fills it (false: the
  // record is dropped, as an encode failure on the caller's thread would drop it).
  // For a record whose encoding is the expensive part -- a session holding the text
  // of every unsaved buffer took ~28 ms to encode on the shell thread, every
  // crash-safety flush. Coalesces with Queue on the same path.
  void QueueEncoded(const std::filesystem::path& path, std::function<bool(std::vector<std::byte>*)> encode,
                    std::uint32_t capability_flags);

  // Block until every write queued so far has been applied. Call before reading a
  // record back, before deleting one, and at shutdown.
  void Flush();

  // Record what `path` was observed to hold on disk (after a successful load), so
  // a later save of unchanged state is recognized as already-persisted and skips
  // the write entirely.
  void NoteOnDiskBody(const std::filesystem::path& path, const std::vector<std::byte>& body);

  // Remove the record at `path` (primary AND `.bak`) on the worker.
  //
  // Queued on the SAME key as writes to that path, which is what makes it safe
  // without blocking: a still-queued write for this record is superseded by the
  // delete rather than racing it, and a write already mid-flight finishes first
  // because the worker is serial. Deleting on the caller's thread instead would
  // have to Flush() the whole queue to get that ordering -- measured at 0.48 ms
  // per delete on the shell thread, which is the stall this class exists to
  // remove.
  void QueueDelete(const std::filesystem::path& path);

  // Total writes actually applied to disk. Lets a test assert that redundant
  // saves coalesced instead of trusting that they did.
  std::size_t applied_write_count() const;

  // Whether every write, encode or delete queued so far has finished on the
  // worker (landed, been skipped as already on disk, or failed). Coalescing does
  // not strand it: a superseded job's replacement carries the later sequence
  // number, and that one runs. Lets the control channel say when a crash-safety
  // flush is actually on disk instead of a driver guessing with a sleep.
  bool settled() const {
    return completed_sequence_.load(std::memory_order_acquire) == submitted_sequence_.load(std::memory_order_acquire);
  }

 private:
  // The worker's own record of what it last successfully wrote per path.
  //
  // This memo lives with the WRITER, not the caller, and that placement is the
  // point: a failed write simply leaves the memo un-updated, so the next save of
  // the same body writes again instead of being skipped as already-persisted.
  // A caller-side memo cannot do that once the result is asynchronous -- it would
  // have already recorded success for a write that then failed, and the state
  // would be lost until the body happened to change.
  static constexpr std::size_t kMaxMemoizedBodyBytes = 1024ull * 1024;

  // The worker's half of Queue / QueueEncoded: skip a body already on disk, else
  // write it and remember it.
  void ApplyWrite(const std::filesystem::path& path, const std::string& key, std::vector<std::byte> body,
                  std::uint32_t capability_flags);

  mutable std::mutex memo_mutex_;
  std::unordered_map<std::string, std::vector<std::byte>> written_body_;
  std::size_t applied_writes_ = 0;

  // Sequence numbers for settled(): one per Post, taken on the caller's thread;
  // the worker publishes the highest it has finished. Jobs run in order, so the
  // finished number only rises.
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

}  // namespace microide::persistence
