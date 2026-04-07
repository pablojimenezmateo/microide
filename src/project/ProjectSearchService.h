#pragma once

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace microide::project {

struct ProjectSearchResult {
  std::filesystem::path relative_path;
  std::size_t line = 0;
  std::size_t column = 0;
  std::string preview;
};

struct ProjectSearchUpdate {
  std::uint64_t run_id = 0;
  std::vector<ProjectSearchResult> results;
  bool finished = false;
  std::string error;
};

class ProjectSearchService {
 public:
  ~ProjectSearchService();

  void SetWakeEventType(Uint32 event_type);
  std::uint64_t Start(const std::filesystem::path& root, std::string query, bool show_hidden);
  void Stop();
  ProjectSearchUpdate TakePendingUpdate();

 private:
  enum class RipgrepOutcome {
    Completed,
    Unavailable,
    Failed,
  };

  void WorkerMain(std::filesystem::path root,
                  std::string query,
                  bool show_hidden,
                  std::uint64_t run_id);
  RipgrepOutcome RunRipgrep(const std::filesystem::path& root,
                            const std::string& query,
                            bool show_hidden,
                            std::uint64_t run_id,
                            std::string& error);
  std::string RunFallbackSearch(const std::filesystem::path& root,
                                const std::string& query,
                                std::uint64_t run_id);
  void PublishResults(std::uint64_t run_id, std::vector<ProjectSearchResult> batch);
  void PublishFinished(std::uint64_t run_id, std::string error = {});
  void PushWakeEvent() const;
  bool StopRequested() const;
  void SetActivePid(int pid);

  mutable std::mutex mutex_;
  std::thread worker_;
  std::atomic<bool> stop_requested_{false};
  std::uint64_t next_run_id_ = 0;
  int active_pid_ = -1;
  Uint32 wake_event_type_ = 0;
  ProjectSearchUpdate pending_update_;
};

}  // namespace microide::project
