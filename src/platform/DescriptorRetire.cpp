#include "platform/DescriptorRetire.h"
#include "util/ThreadName.h"

#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include <unistd.h>

namespace microide::platform {

namespace {

int CloseDescriptor(int fd) {
  return ::close(fd);
}

class DescriptorRetirer {
 public:
  static DescriptorRetirer& Instance() {
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
  DescriptorRetirer() {
    std::thread([this]() {
      util::NameCurrentThread("fd-retire");
      Run();
    }).detach();
  }

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

}

void RetireDescriptorAsync(int fd) {
  if (fd < 0) {
    return;
  }
  DescriptorRetirer::Instance().Retire(fd);
}

}
