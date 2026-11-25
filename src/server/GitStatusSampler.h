#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <thread>

#include "project/GitRepositoryMetadataTracker.h"
#include "util/WakePipe.h"

namespace microide::server {

class GitStatusSampler {
 public:
  GitStatusSampler(std::filesystem::path root, std::chrono::milliseconds safety_interval,
                   std::function<void()> changed);
  ~GitStatusSampler();
  GitStatusSampler(const GitStatusSampler&) = delete;
  GitStatusSampler& operator=(const GitStatusSampler&) = delete;

 private:
  void Run(std::chrono::milliseconds safety_interval);
  void Sample();

  project::GitRepositoryMetadataTracker tracker_;
  std::function<void()> changed_;
  std::atomic<bool> stopping_{false};
  util::WakePipe stop_;
  std::thread thread_;
};

}
