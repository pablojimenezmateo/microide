#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "terminal/TerminalCell.h"
#include "terminal/TerminalInput.h"

namespace microide::platform {
class ProcessLauncher;
}

namespace microide::terminal {

class TerminalSession;

struct HostIdentity {
  std::string epoch;
  std::uint64_t handle = 0;
  bool empty() const { return handle == 0; }
  bool operator==(const HostIdentity&) const = default;
};

enum class HostReattach : std::uint8_t {
  None,
  Pending,
  Reattached,
  ServerRestarted,
  Gone,
};

enum class HostEnd : std::uint8_t {
  LinkLost,
  TerminalGone,
};

class TerminalHostChannel {
 public:
  virtual ~TerminalHostChannel() = default;
  virtual void Send(const TerminalInputEvent& event) = 0;
  virtual void Resize(std::size_t rows, std::size_t columns) = 0;
  virtual void Close() = 0;
  virtual void Detach() = 0;
  virtual HostIdentity identity() const { return {}; }
  virtual std::uint64_t handle() const { return identity().handle; }
  virtual HostReattach reattach() const { return HostReattach::None; }
  virtual std::optional<std::chrono::milliseconds> RoundTrip() const { return std::nullopt; }
  using HistoryDone = std::function<void(std::uint64_t first, std::vector<TerminalLine> lines)>;
  virtual bool FetchHistory(std::uint64_t before, std::size_t count, HistoryDone done) {
    (void)before;
    (void)count;
    (void)done;
    return false;
  }
};

class HostTerminalSource {
 public:
  struct OpenRequest {
    std::filesystem::path working_directory;
    std::string command;
    std::vector<std::string> shell;
    std::size_t rows = 24;
    std::size_t columns = 80;
    std::size_t scrollback_lines = 2000;
    std::size_t credit_bytes = 0;
    std::size_t prefetch_lines = 0;
    HostIdentity attach;
  };

  virtual ~HostTerminalSource() = default;
  virtual bool Ready() const = 0;
  virtual std::shared_ptr<TerminalHostChannel> OpenTerminal(const OpenRequest& request,
                                                            TerminalSession& session,
                                                            std::string* error) const = 0;
};

const HostTerminalSource* HostTerminalsFor(const platform::ProcessLauncher& launcher);

}
