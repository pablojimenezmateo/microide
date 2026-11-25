#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "project/remote/RemoteFrame.h"
#include "util/JsonValue.h"
#include "util/WakePipe.h"

namespace microide::server {

class ProcessTable {
 public:
  struct Sink {
    std::function<void(std::uint64_t connection, project::remote::FrameType type,
                       std::uint64_t handle, std::string_view bytes)>
        content;
    std::function<void(std::uint64_t connection, std::string_view method, std::uint64_t handle,
                        const util::JsonValue& params)>
        notify;
    std::function<std::uint64_t(const std::string& cwd)> git_exited;
  };

  struct Limits {
    std::size_t credit_bytes = 256 * 1024;
    std::size_t retention_bytes = 16u * 1024 * 1024;
    std::size_t max_processes = 256;
    std::chrono::milliseconds keep_grace{30 * 60 * 1000};
  };

  struct SpawnRequest {
    std::vector<std::string> argv;
    std::string cwd;
    std::vector<std::pair<std::string, std::optional<std::string>>> env;
    bool keep_on_detach = false;
    std::string root;
  };
  struct SpawnResult {
    std::uint64_t handle = 0;
    int pid = -1;
    std::string error;
  };

  ProcessTable(Sink sink, Limits limits);
  ProcessTable(Sink sink) : ProcessTable(std::move(sink), Limits{}) {}
  ~ProcessTable();
  ProcessTable(const ProcessTable&) = delete;
  ProcessTable& operator=(const ProcessTable&) = delete;

  static std::optional<SpawnRequest> ParseSpawn(const util::JsonValue& params, std::string* error);

  SpawnResult Spawn(std::uint64_t connection, SpawnRequest request);
  bool WriteStdin(std::uint64_t handle, std::string_view bytes);
  bool CloseStdin(std::uint64_t handle);
  bool Signal(std::uint64_t handle, int signal);
  void Ack(std::uint64_t handle, std::uint64_t stdout_offset, std::uint64_t stderr_offset);
  bool Attach(std::uint64_t connection, std::uint64_t handle, std::uint64_t stdout_offset,
              std::uint64_t stderr_offset, std::string* error);
  void Release(std::uint64_t handle);
  bool End(std::uint64_t handle);
  void Detach(std::uint64_t connection);

  std::size_t LiveCount() const;

  struct Listing {
    std::uint64_t handle = 0;
    std::string root;
    std::vector<std::string> argv;
    int pid = -1;
    std::string cwd;
    bool kept = false;
    std::optional<std::chrono::milliseconds> detached_for;
    bool exited = false;
    std::size_t buffered_bytes = 0;
  };
  static constexpr std::size_t kListedArgs = 8;
  static constexpr std::size_t kListedArgBytes = 256;
  std::vector<Listing> List() const;

 private:
  struct Stream {
    int fd = -1;
    bool eof = false;
    std::string pending;
    std::uint64_t base = 0;
    std::uint64_t sent = 0;
    std::uint64_t total() const { return base + pending.size(); }
  };
  struct Process {
    std::uint64_t handle = 0;
    int pid = -1;
    int pid_fd = -1;
    int stdin_fd = -1;
    Stream out;
    Stream err;
    bool keep_on_detach = false;
    std::uint64_t connection = 0;
    bool git_mutation = false;
    std::string cwd;
    std::string root;
    std::vector<std::string> argv;
    bool exited = false;
    int exit_code = -1;
    int exit_signal = 0;
    bool exit_sent = false;
    std::optional<std::chrono::steady_clock::time_point> kill_at;
    std::optional<std::chrono::steady_clock::time_point> exited_at;
    std::optional<std::chrono::steady_clock::time_point> detached_at;
  };
  struct Outgoing {
    std::uint64_t connection = 0;
    bool is_exit = false;
    std::optional<std::string> git_cwd;
    project::remote::FrameType type = project::remote::FrameType::ProcStdout;
    std::uint64_t handle = 0;
    std::string bytes;
    util::JsonValue params;
  };

  void Run();
  bool Readable(const Stream& stream) const;
  void Pump(Process& process, std::vector<Outgoing>& outgoing);
  void Flush(std::vector<Outgoing>& outgoing);
  void Reap();

  Sink sink_;
  Limits limits_;
  util::WakePipe wake_;
  std::thread thread_;
  std::atomic<bool> stop_{false};

  mutable std::mutex mutex_;
  std::uint64_t next_handle_ = 1;
  std::map<std::uint64_t, std::unique_ptr<Process>> processes_;
};

}
