#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace microide::platform {

class OpenFileWatcher {
 public:
  using Callback = std::function<void(std::vector<std::filesystem::path>)>;

  explicit OpenFileWatcher(Callback callback);
  ~OpenFileWatcher();

  OpenFileWatcher(const OpenFileWatcher&) = delete;
  OpenFileWatcher& operator=(const OpenFileWatcher&) = delete;

  std::uint64_t files_signature() const { return signature_.load(std::memory_order_acquire); }

  void SetFiles(std::uint64_t signature, const std::vector<std::filesystem::path>& files);

 private:
  struct Entry {
    std::string name;
    std::filesystem::path reported;
  };
  struct Waiting {
    std::filesystem::path file;
    std::filesystem::path reported;
  };
  using EntriesByWatch = std::unordered_map<int, std::vector<Entry>>;
  using WaitingByWatch = std::unordered_map<int, std::vector<Waiting>>;
  enum class Armed { Watching, Waiting, Unwatchable };

  bool StartLocked();
  void WorkerMain();
  Armed ArmLocked(const std::filesystem::path& file, const std::filesystem::path& reported,
                  EntriesByWatch& entries, WaitingByWatch& waiting);
  void RearmWaitingLocked(std::vector<std::filesystem::path>& changed);
  void DropUnusedWatchesLocked(const EntriesByWatch& before_entries,
                               const WaitingByWatch& before_waiting);

  Callback callback_;
  std::atomic<std::uint64_t> signature_{0};
  std::mutex mutex_;
  int inotify_fd_ = -1;
  int control_pipe_[2] = {-1, -1};
  EntriesByWatch entries_by_watch_;
  WaitingByWatch waiting_by_watch_;
  std::thread worker_;
};

}
