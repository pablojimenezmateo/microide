#include "project/FileWriteService.h"

#include <utility>

#include "util/PerformanceTrace.h"

namespace microide::project {

FileWriteService::~FileWriteService() { WaitForWrites(); }

void FileWriteService::Begin(Write write) {
  pending_.fetch_add(1, std::memory_order_acq_rel);
  executor_.Submit([this, write = std::move(write)](const util::CancellationToken&) {
    Continuation continuation;
    {
      util::PerformanceTrace::Scope scope("FileWriteService::Write");
      continuation = write();
    }
    if (continuation) {
      mailbox_.Post(std::move(continuation));
    }
    {
      const std::lock_guard<std::mutex> lock(idle_mutex_);
      pending_.fetch_sub(1, std::memory_order_acq_rel);
    }
    idle_.notify_all();
  });
}

void FileWriteService::WaitForWrites() {
  std::unique_lock<std::mutex> lock(idle_mutex_);
  idle_.wait(lock, [this] { return pending_.load(std::memory_order_acquire) == 0; });
}

void FileWriteService::Flush() {
  WaitForWrites();
  (void)mailbox_.Drain();
}

}
