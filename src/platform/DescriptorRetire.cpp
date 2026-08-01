#include "platform/DescriptorRetire.h"

#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace microide::platform {

namespace {

int CloseDescriptor(int fd) {
#if defined(_WIN32)
  return ::_close(fd);
#else
  return ::close(fd);
#endif
}

// One process-lifetime worker draining a queue of descriptors.
//
// It is deliberately the smallest possible amount of shared state: a vector of
// ints. It holds no reference to any application object, so it cannot dangle
// against one and cannot couple two subsystems the way a shared service would.
// A detached thread per retire would work equally well; a single worker just
// avoids unbounded thread creation if a caller ever retires in a loop.
//
// It never stops. The process leaves via std::quick_exit, which runs neither
// atexit handlers nor static destructors, so there is no shutdown point at which
// joining it would be possible or useful -- and any descriptor still queued is
// closed by the kernel on exit anyway.
class DescriptorRetirer {
 public:
  static DescriptorRetirer& Instance() {
    // Deliberately never destroyed. A function-local static *object* would be
    // destroyed by an atexit handler in any entry point that leaves through a
    // normal exit (the test and perf binaries do; the app calls quick_exit), and
    // the detached worker below would then wake on a destroyed mutex. The static
    // pointer is trivially destructible, so nothing runs at exit; it also keeps
    // the allocation reachable from a root, so a leak checker does not flag it.
    static DescriptorRetirer* retirer = new DescriptorRetirer();
    return *retirer;
  }

  void Retire(int fd) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      pending_.push_back(fd);
    }
    condition_.notify_one();
  }

 private:
  DescriptorRetirer() { std::thread([this]() { Run(); }).detach(); }

  void Run() {
    std::vector<int> batch;
    while (true) {
      {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [this]() { return !pending_.empty(); });
        batch.swap(pending_);
      }
      for (const int fd : batch) {
        CloseDescriptor(fd);
      }
      batch.clear();
    }
  }

  std::mutex mutex_;
  std::condition_variable condition_;
  std::vector<int> pending_;
};

}  // namespace

void RetireDescriptorAsync(int fd) {
  if (fd < 0) {
    return;
  }
  DescriptorRetirer::Instance().Retire(fd);
}

}  // namespace microide::platform
