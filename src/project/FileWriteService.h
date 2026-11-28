#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>

#include "util/MainThreadMailbox.h"
#include "util/TaskExecutor.h"
#include "util/Waker.h"

namespace microide::project {

class FileWriteService {
 public:
  using Continuation = std::function<void()>;
  using Write = std::function<Continuation()>;

  FileWriteService() = default;
  FileWriteService(const FileWriteService&) = delete;
  FileWriteService& operator=(const FileWriteService&) = delete;
  ~FileWriteService();

  void SetWakeChannel(util::WakeChannel channel) { mailbox_.SetWakeChannel(channel); }

  void Begin(Write write);

  int DrainCompletions() { return mailbox_.Drain(); }

  [[nodiscard]] bool HasCompletions() const { return mailbox_.PendingCount() > 0; }

  [[nodiscard]] int PendingCount() const { return pending_.load(std::memory_order_acquire); }

  void Flush();

 private:
  void WaitForWrites();

  util::MainThreadMailbox mailbox_;
  std::atomic<int> pending_{0};
  std::mutex idle_mutex_;
  std::condition_variable idle_;
  util::TaskExecutor executor_{1};
};

}
