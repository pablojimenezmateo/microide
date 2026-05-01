#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "platform/Subprocess.h"

namespace microide::platform {

class AsyncProcessBackend {
 public:
  virtual ~AsyncProcessBackend() = default;

  virtual bool Start(const std::vector<std::string>& argv, const std::string& cwd) = 0;
  virtual bool IsRunning() = 0;
  virtual bool Write(std::string_view data) = 0;
  virtual std::optional<std::string> Read(std::size_t max_bytes, int timeout_ms) = 0;
  virtual std::optional<std::string> ReadExact(std::size_t n, int timeout_ms) = 0;
  virtual void Shutdown(int timeout_ms) = 0;
  virtual int pid() const = 0;
  virtual std::optional<int> exit_code() const = 0;
};

SubprocessResult RunSubprocessWithBackend(const std::vector<std::string>& argv,
                                         const SubprocessOptions& options);
std::unique_ptr<AsyncProcessBackend> CreateAsyncProcessBackend();

}  // namespace microide::platform
