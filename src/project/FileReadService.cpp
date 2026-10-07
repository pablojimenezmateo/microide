#include "project/FileReadService.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <utility>

#include "util/PerformanceTrace.h"

namespace microide::project {
namespace {

// Chunk size for the cancellable read. Big enough that a 200 MB file is a few
// hundred reads rather than a few hundred thousand, small enough that a cancel
// lands within a millisecond or so of being asked for even on a slow device.
constexpr std::size_t kReadChunkBytes = 1u << 20;  // 1 MiB

}  // namespace

std::shared_ptr<std::atomic<bool>> FileReadService::TrackRequest(std::uint64_t id) {
  auto cancelled = std::make_shared<std::atomic<bool>>(false);
  const std::lock_guard<std::mutex> lock(requests_mutex_);
  requests_.push_back(InFlightRead{id, cancelled});
  return cancelled;
}

FileReadService::~FileReadService() {
  const std::lock_guard<std::mutex> lock(requests_mutex_);
  for (const InFlightRead& read : requests_) {
    read.cancelled->store(true, std::memory_order_release);
  }
}

void FileReadService::ForgetRequest(std::uint64_t id) {
  const std::lock_guard<std::mutex> lock(requests_mutex_);
  const auto it = std::find_if(requests_.begin(), requests_.end(),
                               [id](const InFlightRead& read) { return read.id == id; });
  if (it != requests_.end()) {
    *it = std::move(requests_.back());
    requests_.pop_back();
  }
}

std::uint64_t FileReadService::Begin(Request request) {
  const std::uint64_t id = next_id_.fetch_add(1, std::memory_order_acq_rel) + 1;
  std::shared_ptr<std::atomic<bool>> cancelled = TrackRequest(id);
  pending_.fetch_add(1, std::memory_order_acq_rel);

  // Everything the worker touches is copied into the task: the path, the cap and
  // the cancel flag. It reads no shell state and no viewport, which is what makes
  // running it off-thread safe at all.
  executor_.Submit([this, id, path = std::move(request.path), max_bytes = request.max_bytes,
                    read_path = request.read_path, cancelled = std::move(cancelled), on_worker = std::move(request.on_worker),
                    on_complete = std::move(request.on_complete)](
                       const util::CancellationToken&) mutable {
    util::PerformanceTrace::Scope perf_scope("FileReadService::Read");
    Completion completion;
    completion.id = id;
    completion.path = path;
    completion.status = Status::Unreadable;

    const auto is_cancelled = [&cancelled] {
      return cancelled->load(std::memory_order_acquire);
    };

    if (is_cancelled()) {
      completion.status = Status::Cancelled;
    } else if (!read_path) {
      completion.status = Status::Ok;  // nothing to read; the hook does the work
    } else if (!util::IsRegularFileFollowingSymlinks(path)) {
      // A FIFO, device node or procfs entry can block on open or seek before any
      // size guard runs, so it is rejected up front — the same rule every
      // synchronous text-read entry point applies.
      completion.status = Status::Unreadable;
    } else if (std::ifstream file(path, std::ios::binary); !file) {
      completion.status = Status::Unreadable;
    } else {
      file.seekg(0, std::ios::end);
      const std::streamoff size = file.tellg();
      if (size < 0) {
        completion.status = Status::Unreadable;
      } else if (static_cast<std::uintmax_t>(size) > max_bytes) {
        // Refused before allocating: a multi-GB or sparse file would otherwise
        // force one enormous allocation and an uncaught bad_alloc.
        completion.status = Status::TooLarge;
      } else {
        file.seekg(0, std::ios::beg);
        completion.bytes.resize(static_cast<std::size_t>(size));
        std::size_t read_so_far = 0;
        bool failed = false;
        while (read_so_far < completion.bytes.size()) {
          if (is_cancelled()) {
            break;
          }
          const std::size_t chunk =
              std::min(kReadChunkBytes, completion.bytes.size() - read_so_far);
          file.read(completion.bytes.data() + read_so_far, static_cast<std::streamsize>(chunk));
          if (!file) {
            failed = true;
            break;
          }
          read_so_far += chunk;
        }
        if (failed) {
          completion.status = Status::Unreadable;
        } else if (read_so_far < completion.bytes.size()) {
          completion.status = Status::Cancelled;
        } else {
          completion.status = Status::Ok;
        }
      }
    }
    // A partial or refused read must not hand back a buffer that looks like
    // content: the caller branches on `status`, but a half-read file sitting in
    // `bytes` is one missed branch away from being opened as the document.
    if (completion.status != Status::Ok) {
      completion.bytes.clear();
      completion.bytes.shrink_to_fit();
    } else if (on_worker) {
      util::PerformanceTrace::Scope worker_scope("FileReadService::OnWorker");
      on_worker(completion.bytes);
    }

    ForgetRequest(id);
    // Decrement BEFORE posting: CancelAllAndFlush waits on the executor and then
    // drains, so a completion must never be counted as still pending once its
    // task is about to end.
    pending_.fetch_sub(1, std::memory_order_acq_rel);
    mailbox_.Post([completion = std::move(completion),
                   on_complete = std::move(on_complete)]() mutable {
      if (on_complete) {
        on_complete(std::move(completion));
      }
    });
  });
  return id;
}

void FileReadService::Cancel(std::uint64_t id) {
  if (id == 0) {
    return;
  }
  const std::lock_guard<std::mutex> lock(requests_mutex_);
  const auto it = std::find_if(requests_.begin(), requests_.end(),
                               [id](const InFlightRead& read) { return read.id == id; });
  if (it != requests_.end()) {
    it->cancelled->store(true, std::memory_order_release);
  }
}

void FileReadService::FlushPendingReads() {
  if (PendingCount() == 0 && mailbox_.PendingCount() == 0) {
    return;
  }
  util::PerformanceTrace::Scope perf_scope("FileReadService::FlushPendingReads");
  // WaitForIdle covers both the queued and the running task; the mailbox then
  // holds every completion those tasks posted, so draining after the wait is
  // what turns a deferred open back into a completed one.
  executor_.WaitForIdle();
  mailbox_.Drain();
}

void FileReadService::CancelAllAndFlush() {
  {
    const std::lock_guard<std::mutex> lock(requests_mutex_);
    for (InFlightRead& read : requests_) {
      read.cancelled->store(true, std::memory_order_release);
    }
  }
  if (PendingCount() == 0 && mailbox_.PendingCount() == 0) {
    return;
  }
  util::PerformanceTrace::Scope perf_scope("FileReadService::CancelAllAndFlush");
  executor_.WaitForIdle();
  mailbox_.Drain();
}

}  // namespace microide::project
