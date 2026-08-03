#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
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

  mutable std::mutex memo_mutex_;
  std::unordered_map<std::string, std::vector<std::byte>> written_body_;
  std::size_t applied_writes_ = 0;

  util::SerialWorkQueue queue_;
};

}  // namespace microide::persistence
