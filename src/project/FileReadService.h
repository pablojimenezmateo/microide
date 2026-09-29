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

// Reads whole files OFF the shell thread.
//
// Opening a file reads all of it synchronously, which is right for a source file
// on a warm page cache and wrong for everything else: a 200 MB generated blob, a
// file on a stalled network mount, a cold spinning disk. The read is the one
// place the "every synchronous site is fine locally" assumption genuinely breaks,
// because it is unbounded work with the window frozen behind it.
//
// This is deliberately NOT `ProjectBackgroundExecutor`. That is a serial queue
// shared with git status, blame and search; a half-gigabyte read parked on it
// stalls all three for as long as it runs. One dedicated thread is enough for a
// workload that is one file at a time, and it means a slow read costs only the
// tab waiting for it.
//
// Cancellation is real, not just "ignore the answer": the read loop checks its
// flag between chunks, so closing the tab stops the I/O rather than finishing a
// read whose result nobody will look at. The completion still arrives (as
// `Cancelled`), so every posted read is accounted for by exactly one completion.
class FileReadService {
 public:
  enum class Status : std::uint8_t {
    Ok,
    Cancelled,   // the tab was closed or retargeted while the read ran
    Unreadable,  // absent, not a regular file, or an I/O failure
    TooLarge,    // above the whole-file cap; refused before allocating
  };

  struct Completion {
    std::uint64_t id = 0;
    std::filesystem::path path;
    Status status = Status::Unreadable;
    // The file's bytes, verbatim. Classification (encoding, BOM, line endings)
    // happens where it always has, on the shell thread, against these bytes.
    std::string bytes;

    [[nodiscard]] bool ok() const { return status == Status::Ok; }
  };

  FileReadService() = default;
  FileReadService(const FileReadService&) = delete;
  FileReadService& operator=(const FileReadService&) = delete;

  void SetWakeChannel(util::WakeChannel channel) { mailbox_.SetWakeChannel(channel); }

  struct Request {
    std::filesystem::path path;
    // Runs ON THE WORKER with the bytes just read, before the completion is
    // posted — and only when the read succeeded. The byte-shaped half of loading
    // a file (a content hash, an encoding sniff, a line-ending scan, a CRLF
    // rewrite) is several full passes over the buffer, and it belongs on this
    // side of the handoff rather than on the shell thread with a frame waiting.
    // It may move the bytes out. It must touch nothing but what it is given and
    // what it owns; anything shell-owned read here is a data race.
    std::function<void(std::string&)> on_worker;
    // Runs on the SHELL thread, from `DrainCompletions` or `FlushPendingReads`,
    // exactly once per posted read whatever became of it.
    //
    // It travels with the request rather than being bound once, for the same
    // reason `SaveFormatterService` does it: a callback installed during
    // initialization is a callback a path that skipped initialization does not
    // have, and a dropped completion here leaves a tab loading forever.
    std::function<void(Completion)> on_complete;
    std::uintmax_t max_bytes = util::kMaxTextFileBytes;
  };

  // Post one read. Returns the id its completion will carry; never 0.
  std::uint64_t Begin(Request request);

  // Stop a read whose result is no longer wanted. Its completion still arrives,
  // with `Cancelled`. Unknown or already-finished ids are ignored.
  void Cancel(std::uint64_t id);

  // Shell thread: deliver every completion that has landed. Returns how many ran.
  int DrainCompletions() { return mailbox_.Drain(); }

  // Shell thread: wait for every outstanding read and deliver its completion.
  // For a caller that cannot proceed with a tab still loading — a save sweep, a
  // session flush — where the answer is wanted rather than abandoned.
  void FlushPendingReads();

  // Shell thread: cancel everything outstanding, wait for the worker, and deliver
  // the completions. For teardown — closing a project, quitting — where a tab
  // that is still loading is about to stop existing, so finishing its read is
  // pure latency on the way out.
  void CancelAllAndFlush();

  [[nodiscard]] int PendingCount() const { return pending_.load(std::memory_order_acquire); }

  // Reads posted over this service's lifetime. Monotonic, so a test can assert
  // that work went off-thread at all without racing whether it has finished —
  // which is the difference between checking the mechanism and checking the
  // clock.
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

  // One worker. See the class comment: the workload is one file at a time, and a
  // second thread would only let two reads compete for the same disk.
  util::TaskExecutor executor_{1};
  util::MainThreadMailbox mailbox_;
  std::atomic<int> pending_{0};
  std::atomic<std::uint64_t> next_id_{0};
  mutable std::mutex requests_mutex_;
  std::vector<InFlightRead> requests_;
};

}  // namespace microide::project
