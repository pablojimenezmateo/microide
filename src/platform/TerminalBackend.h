#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace microide::platform {

struct TerminalStartRequest {
  std::filesystem::path working_directory;
  std::string command;
  // Shell program to launch when `command` is empty (the `terminal.shell`
  // setting). Empty falls back to the platform default ($SHELL / /bin/sh).
  std::string shell;
  std::size_t rows = 24;
  std::size_t columns = 80;
};

struct TerminalStartResult {
  bool started = false;
  bool running = false;
  int child_process_id = -1;
  std::string launch_label;
  std::string initial_output;
};

struct TerminalBackendCallbacks {
  std::function<void(std::string_view)> on_output;
  std::function<void()> on_exit;
};

class TerminalBackend {
 public:
  virtual ~TerminalBackend() = default;

  virtual TerminalStartResult Start(const TerminalStartRequest& request,
                                    TerminalBackendCallbacks callbacks) = 0;
  virtual void Stop() = 0;
  virtual void Resize(std::size_t rows, std::size_t columns) = 0;
  virtual void Write(std::string_view bytes) = 0;
  virtual bool running() const = 0;
};

std::unique_ptr<TerminalBackend> CreateTerminalBackend();

}  // namespace microide::platform
