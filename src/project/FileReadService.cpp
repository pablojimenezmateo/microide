#include "project/FileReadService.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <utility>

#include "util/PerformanceTrace.h"

namespace microide::project {
namespace {

constexpr std::size_t kReadChunkBytes = 1u << 20;

}

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
      completion.status = Status::Ok;
    } else if (!util::IsRegularFileFollowingSymlinks(path)) {
      completion.status = Status::Unreadable;
    } else if (std::ifstream file(path, std::ios::binary); !file) {
      completion.status = Status::Unreadable;
    } else {
      file.seekg(0, std::ios::end);
      const std::streamoff size = file.tellg();
      if (size < 0) {
        completion.status = Status::Unreadable;
      } else if (static_cast<std::uintmax_t>(size) > max_bytes) {
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
    if (completion.status != Status::Ok) {
      completion.bytes.clear();
      completion.bytes.shrink_to_fit();
    } else if (on_worker) {
      util::PerformanceTrace::Scope worker_scope("FileReadService::OnWorker");
      on_worker(completion.bytes);
    }

    ForgetRequest(id);
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
  util::PerformanceTrace::Scope perf_scope("FileReadService::FlushPendingReads");
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
  util::PerformanceTrace::Scope perf_scope("FileReadService::CancelAllAndFlush");
  executor_.WaitForIdle();
  mailbox_.Drain();
}

}
