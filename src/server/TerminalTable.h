#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
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

#include "terminal/TerminalHostFrameBuilder.h"
#include "terminal/TerminalHostWire.h"
#include "terminal/TerminalSession.h"
#include "util/JsonValue.h"

namespace microide::server {

class TerminalTable {
 public:
  struct Sink {
    std::function<void(std::uint64_t connection, std::uint64_t handle, std::string_view frame)>
        frame;
  };

  struct Limits {
    std::size_t credit_bytes = 256 * 1024;
    std::size_t prefetch_lines = 500;
    std::size_t max_terminals = 64;
    std::chrono::milliseconds frame_interval{16};
    std::chrono::milliseconds congested_interval{33};
    std::chrono::milliseconds echo_timeout{50};
  };

  struct OpenRequest {
    std::string cwd;
    std::string command;
    std::vector<std::string> shell;
    std::size_t rows = 24;
    std::size_t columns = 80;
    std::size_t scrollback_lines = 2000;
    std::size_t credit_bytes = 0;
    std::size_t prefetch_lines = 0;
    std::string root;
    terminal::TerminalHostWireOptions wire;
  };
  struct OpenResult {
    std::uint64_t handle = 0;
    std::size_t credit_bytes = 0;
    std::string error;
  };

  TerminalTable(Sink sink, Limits limits);
  explicit TerminalTable(Sink sink) : TerminalTable(std::move(sink), Limits{}) {}
  ~TerminalTable();
  TerminalTable(const TerminalTable&) = delete;
  TerminalTable& operator=(const TerminalTable&) = delete;

  static std::optional<OpenRequest> ParseOpen(const util::JsonValue& params, std::string* error);

  OpenResult Open(std::uint64_t connection, OpenRequest request);
  struct Resume {
    std::uint64_t screen_top = 0;
    bool alternate = false;
  };
  bool Attach(std::uint64_t connection, std::uint64_t handle, terminal::TerminalHostWireOptions wire,
              std::optional<Resume> resume = {});
  void Input(std::uint64_t connection, std::uint64_t handle,
             const std::vector<terminal::TerminalInputEvent>& events);
  void Resize(std::uint64_t connection, std::uint64_t handle, std::size_t rows, std::size_t columns);
  void Ack(std::uint64_t connection, std::uint64_t handle, std::uint64_t received_bytes);
  bool History(std::uint64_t handle, std::uint64_t before, std::size_t count,
               std::uint64_t* first, std::vector<terminal::TerminalLine>& lines);
  void Close(std::uint64_t connection, std::uint64_t handle);
  void Detach(std::uint64_t connection);
  void Detach(std::uint64_t connection, std::uint64_t handle);
  bool RequestOpenUrl(std::uint64_t handle, std::string url);
  std::uint64_t RequestEdit(std::uint64_t handle, terminal::TerminalHostEdit edit);
  void SetShellEnvironment(std::vector<std::string> environment);

  std::size_t LiveCount() const;
  const Limits& limits() const { return limits_; }

  struct Listing {
    std::uint64_t handle = 0;
    std::string root;
    std::string cwd;
    std::size_t clients = 0;
    bool exited = false;
    int pid = -1;
    std::size_t scrollback_lines = 0;
  };
  std::vector<Listing> List() const;

 private:
  struct Client {
    std::uint64_t connection = 0;
    terminal::TerminalHostFrameBuilder builder;
    terminal::TerminalHostWireOptions wire;
    std::size_t rows = 0;
    std::size_t columns = 0;
    std::chrono::steady_clock::time_point last_sent{};
    std::uint64_t sent_bytes = 0;
    std::uint64_t acked_bytes = 0;
    std::uint64_t written_seq = 0;
    std::uint64_t echo_ack = 0;
    std::chrono::steady_clock::time_point written_at{};
    std::uint64_t seen_generation = 0;
    std::uint64_t written_generation = 0;
    std::uint64_t written_output = 0;
    bool bell = false;
    std::optional<std::string> clipboard;
    std::optional<terminal::TerminalHostNotification> notification;
    std::optional<std::string> open_url;
    std::optional<terminal::TerminalHostEdit> edit;
    bool signal_pending() const { return bell || clipboard || notification || open_url || edit; }
  };

  struct Terminal {
    std::uint64_t handle = 0;
    terminal::TerminalSession session;
    std::size_t credit_bytes = 0;
    std::size_t prefetch_lines = 0;
    std::string root;
    std::vector<Client> clients;
    std::atomic<std::uint64_t> output_generation{1};

    Client* FindClient(std::uint64_t connection);
  };

  void ApplySharedSizeLocked(Terminal& terminal);
  bool RemoveClientLocked(Terminal& terminal, std::uint64_t connection);
  void Run();
  void Notify();

  Sink sink_;
  Limits limits_;
  std::thread thread_;

  mutable std::mutex mutex_;
  std::condition_variable wake_;
  bool woken_ = false;
  bool stop_ = false;
  std::uint64_t next_handle_ = 1;
  std::vector<std::string> shell_environment_;
  std::map<std::uint64_t, std::shared_ptr<Terminal>> terminals_;
};

}
