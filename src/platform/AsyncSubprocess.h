#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "platform/SubprocessSandbox.h"

namespace microide::platform {

class AsyncProcessController {
 public:
  virtual ~AsyncProcessController() = default;
  virtual bool IsRunning() const = 0;
  virtual std::optional<int> exit_code() const = 0;
  virtual int pid() const = 0;
  virtual void Terminate(int timeout_ms) = 0;
};

class AsyncSubprocess {
 public:
  AsyncSubprocess();
  ~AsyncSubprocess();
  AsyncSubprocess(const AsyncSubprocess&) = delete;
  AsyncSubprocess& operator=(const AsyncSubprocess&) = delete;
  AsyncSubprocess(AsyncSubprocess&&) noexcept;
  AsyncSubprocess& operator=(AsyncSubprocess&&) noexcept;

  bool Start(const std::vector<std::string>& argv, const std::string& cwd = {},
             const SubprocessSandbox& sandbox = {}, int stderr_fd = -1);

  bool Adopt(int stdin_fd, int stdout_fd, std::shared_ptr<AsyncProcessController> controller);

  bool IsRunning() const;

  bool Write(std::string_view data);
  std::optional<std::size_t> WriteSome(std::string_view data);

  std::optional<std::string> Read(std::size_t max_bytes = 65536, int timeout_ms = 1000);


  void CloseStdin();

  void Shutdown(int timeout_ms = 3000);

  void Finish(int grace_ms, int kill_timeout_ms = 3000);

  int pid() const;

  int stdout_fd() const;
  int stdin_fd() const;

  std::optional<int> exit_code() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}
