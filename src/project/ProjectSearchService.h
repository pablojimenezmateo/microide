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

enum class ProjectSearchPatternMode {
  Literal,
  Regex,
};

enum class ProjectSearchCaseMode {
  Smart,
  Sensitive,
  Insensitive,
};

struct ProjectSearchOptions {
  ProjectSearchPatternMode pattern_mode = ProjectSearchPatternMode::Literal;
  ProjectSearchCaseMode case_mode = ProjectSearchCaseMode::Smart;
  bool show_hidden = false;
};

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
  std::uint64_t Start(const std::filesystem::path& root,
                      std::string query,
                      ProjectSearchOptions options = {});
  void Stop();
  ProjectSearchUpdate TakePendingUpdate();

 private:
  void WorkerMain(std::filesystem::path root,
                  std::string query,
                  ProjectSearchOptions options,
                  std::uint64_t run_id);
  std::string RunSearch(const std::filesystem::path& root,
                        const std::string& query,
                        const ProjectSearchOptions& options,
                        std::uint64_t run_id);
  void PublishResults(std::uint64_t run_id, std::vector<ProjectSearchResult> batch);
  void PublishFinished(std::uint64_t run_id, std::string error = {});
  void PushWakeEvent() const;
  bool StopRequested() const;

  mutable std::mutex mutex_;
  std::thread worker_;
  std::atomic<bool> stop_requested_{false};
  std::uint64_t next_run_id_ = 0;
  Uint32 wake_event_type_ = 0;
  ProjectSearchUpdate pending_update_;
};

}  // namespace microide::project
