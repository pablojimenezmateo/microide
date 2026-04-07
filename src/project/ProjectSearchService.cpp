#include "project/ProjectSearchService.h"

#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string_view>
#include <vector>

#include "project/FileIndex.h"

namespace microide::project {

namespace {

constexpr std::size_t kBatchSize = 32;
constexpr std::size_t kMaxResults = 200;

bool IsDigits(std::string_view text) {
  return !text.empty() &&
         std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c); });
}

std::string ToLower(std::string_view text) {
  std::string lowered(text);
  std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return lowered;
}

bool ParseVimgrepLine(std::string_view line, ProjectSearchResult& out_result) {
  std::size_t first = line.find(':');
  while (first != std::string_view::npos) {
    const std::size_t second = line.find(':', first + 1);
    if (second == std::string_view::npos) {
      return false;
    }
    const std::size_t third = line.find(':', second + 1);
    if (third == std::string_view::npos) {
      return false;
    }

    const std::string_view line_text = line.substr(first + 1, second - first - 1);
    const std::string_view column_text = line.substr(second + 1, third - second - 1);
    if (IsDigits(line_text) && IsDigits(column_text)) {
      const std::size_t line_number = static_cast<std::size_t>(std::strtoull(
          std::string(line_text).c_str(), nullptr, 10));
      const std::size_t column_number = static_cast<std::size_t>(std::strtoull(
          std::string(column_text).c_str(), nullptr, 10));
      out_result = ProjectSearchResult{
          .relative_path = std::filesystem::path(std::string(line.substr(0, first))).lexically_normal(),
          .line = line_number == 0 ? 0 : line_number - 1,
          .column = column_number == 0 ? 0 : column_number - 1,
          .preview = std::string(line.substr(third + 1)),
      };
      return true;
    }

    first = line.find(':', first + 1);
  }

  return false;
}

}  // namespace

ProjectSearchService::~ProjectSearchService() {
  Stop();
}

void ProjectSearchService::SetWakeEventType(Uint32 event_type) {
  std::lock_guard lock(mutex_);
  wake_event_type_ = event_type;
}

std::uint64_t ProjectSearchService::Start(const std::filesystem::path& root,
                                          std::string query,
                                          bool show_hidden) {
  Stop();

  std::lock_guard lock(mutex_);
  pending_update_ = {};
  const std::uint64_t run_id = ++next_run_id_;
  stop_requested_.store(false);
  worker_ = std::thread(&ProjectSearchService::WorkerMain, this, root, std::move(query), show_hidden,
                        run_id);
  return run_id;
}

void ProjectSearchService::Stop() {
  stop_requested_.store(true);

  int pid = -1;
  {
    std::lock_guard lock(mutex_);
    pid = active_pid_;
  }
  if (pid > 0) {
    kill(pid, SIGKILL);
  }

  if (worker_.joinable()) {
    worker_.join();
  }

  std::lock_guard lock(mutex_);
  active_pid_ = -1;
  pending_update_ = {};
}

ProjectSearchUpdate ProjectSearchService::TakePendingUpdate() {
  std::lock_guard lock(mutex_);
  ProjectSearchUpdate update = std::move(pending_update_);
  pending_update_ = {};
  return update;
}

void ProjectSearchService::WorkerMain(std::filesystem::path root,
                                      std::string query,
                                      bool show_hidden,
                                      std::uint64_t run_id) {
  if (query.empty()) {
    PublishFinished(run_id);
    return;
  }

  std::string error;
  switch (RunRipgrep(root, query, show_hidden, run_id, error)) {
    case RipgrepOutcome::Completed:
      PublishFinished(run_id, std::move(error));
      return;
    case RipgrepOutcome::Unavailable:
      error = RunFallbackSearch(root, query, run_id);
      if (!StopRequested()) {
        PublishFinished(run_id, std::move(error));
      }
      return;
    case RipgrepOutcome::Failed:
      PublishFinished(run_id, std::move(error));
      return;
  }
}

ProjectSearchService::RipgrepOutcome ProjectSearchService::RunRipgrep(const std::filesystem::path& root,
                                                                      const std::string& query,
                                                                      bool show_hidden,
                                                                      std::uint64_t run_id,
                                                                      std::string& error) {
  int pipe_fds[2] = {-1, -1};
  if (pipe(pipe_fds) != 0) {
    error = std::strerror(errno);
    return RipgrepOutcome::Unavailable;
  }

  const pid_t pid = fork();
  if (pid < 0) {
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    error = std::strerror(errno);
    return RipgrepOutcome::Failed;
  }

  if (pid == 0) {
    if (chdir(root.c_str()) != 0) {
      _exit(126);
    }

    dup2(pipe_fds[1], STDOUT_FILENO);
    dup2(pipe_fds[1], STDERR_FILENO);
    close(pipe_fds[0]);
    close(pipe_fds[1]);

    std::vector<std::string> args = {"rg", "--vimgrep", "--smart-case", "--color", "never"};
    if (show_hidden) {
      args.push_back("--hidden");
    }
    args.push_back(query);
    args.push_back(".");

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& arg : args) {
      argv.push_back(arg.data());
    }
    argv.push_back(nullptr);

    execvp("rg", argv.data());
    _exit(127);
  }

  close(pipe_fds[1]);
  SetActivePid(static_cast<int>(pid));

  FILE* stream = fdopen(pipe_fds[0], "r");
  if (stream == nullptr) {
    close(pipe_fds[0]);
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
    SetActivePid(-1);
    error = std::strerror(errno);
    return RipgrepOutcome::Failed;
  }

  std::vector<ProjectSearchResult> batch;
  char* line = nullptr;
  std::size_t capacity = 0;
  while (!StopRequested()) {
    const ssize_t read = getline(&line, &capacity, stream);
    if (read < 0) {
      break;
    }

    std::string_view text(line, static_cast<std::size_t>(read));
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
      text.remove_suffix(1);
    }
    if (text.empty()) {
      continue;
    }

    ProjectSearchResult result;
    if (ParseVimgrepLine(text, result)) {
      batch.push_back(std::move(result));
      if (batch.size() >= kBatchSize) {
        PublishResults(run_id, std::move(batch));
        batch = {};
        if (StopRequested()) {
          break;
        }
      }
      continue;
    }

    if (error.empty()) {
      error = std::string(text);
    }
  }

  if (line != nullptr) {
    free(line);
  }
  fclose(stream);

  int status = 0;
  waitpid(pid, &status, 0);
  SetActivePid(-1);

  if (!batch.empty() && !StopRequested()) {
    PublishResults(run_id, std::move(batch));
  }

  if (StopRequested()) {
    return RipgrepOutcome::Failed;
  }

  if (WIFEXITED(status)) {
    const int code = WEXITSTATUS(status);
    if (code == 0 || code == 1) {
      return RipgrepOutcome::Completed;
    }
    if (code == 127) {
      error.clear();
      return RipgrepOutcome::Unavailable;
    }
    if (error.empty()) {
      error = "rg exited with status " + std::to_string(code);
    }
    return RipgrepOutcome::Failed;
  }

  if (error.empty()) {
    error = "rg terminated unexpectedly";
  }
  return RipgrepOutcome::Failed;
}

std::string ProjectSearchService::RunFallbackSearch(const std::filesystem::path& root,
                                                    const std::string& query,
                                                    std::uint64_t run_id) {
  FileIndex index;
  if (!index.SetRoot(root)) {
    return "Failed to index project files";
  }

  const std::string lowered_query = ToLower(query);
  std::vector<ProjectSearchResult> batch;
  std::size_t total_results = 0;

  for (const auto& relative_path : index.files()) {
    if (StopRequested()) {
      return {};
    }

    std::ifstream file(root / relative_path);
    if (!file) {
      continue;
    }

    std::string line;
    std::size_t line_index = 0;
    while (std::getline(file, line) && !StopRequested()) {
      const std::string lowered_line = ToLower(line);
      std::size_t offset = lowered_line.find(lowered_query);
      while (offset != std::string::npos) {
        batch.push_back(ProjectSearchResult{
            .relative_path = relative_path,
            .line = line_index,
            .column = offset,
            .preview = line,
        });
        ++total_results;
        if (batch.size() >= kBatchSize) {
          PublishResults(run_id, std::move(batch));
          batch = {};
        }
        if (total_results >= kMaxResults) {
          if (!batch.empty()) {
            PublishResults(run_id, std::move(batch));
          }
          return {};
        }
        offset = lowered_line.find(lowered_query, offset + 1);
      }
      ++line_index;
    }
  }

  if (!batch.empty() && !StopRequested()) {
    PublishResults(run_id, std::move(batch));
  }
  return {};
}

void ProjectSearchService::PublishResults(std::uint64_t run_id, std::vector<ProjectSearchResult> batch) {
  if (batch.empty() || StopRequested()) {
    return;
  }

  {
    std::lock_guard lock(mutex_);
    if (pending_update_.run_id != 0 && pending_update_.run_id != run_id) {
      pending_update_ = {};
    }
    pending_update_.run_id = run_id;
    for (auto& result : batch) {
      if (pending_update_.results.size() >= kMaxResults) {
        break;
      }
      pending_update_.results.push_back(std::move(result));
    }
  }
  PushWakeEvent();
}

void ProjectSearchService::PublishFinished(std::uint64_t run_id, std::string error) {
  if (StopRequested()) {
    return;
  }

  {
    std::lock_guard lock(mutex_);
    if (pending_update_.run_id != 0 && pending_update_.run_id != run_id) {
      pending_update_ = {};
    }
    pending_update_.run_id = run_id;
    pending_update_.finished = true;
    pending_update_.error = std::move(error);
  }
  PushWakeEvent();
}

void ProjectSearchService::PushWakeEvent() const {
  std::lock_guard lock(mutex_);
  if (wake_event_type_ == 0) {
    return;
  }

  SDL_Event event;
  SDL_zero(event);
  event.type = wake_event_type_;
  SDL_PushEvent(&event);
}

bool ProjectSearchService::StopRequested() const {
  return stop_requested_.load();
}

void ProjectSearchService::SetActivePid(int pid) {
  std::lock_guard lock(mutex_);
  active_pid_ = pid;
}

}  // namespace microide::project
