#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

namespace microide::platform {

class ControlSocketClient {
 public:
  ControlSocketClient() = default;
  ~ControlSocketClient();
  ControlSocketClient(const ControlSocketClient&) = delete;
  ControlSocketClient& operator=(const ControlSocketClient&) = delete;

  bool Connect(const std::filesystem::path& socket_path);

  bool SendLine(const std::string& line,
                std::chrono::milliseconds timeout = std::chrono::seconds(10));

  void ShutdownWrite();

  std::optional<std::string> ReadLine(std::chrono::milliseconds timeout);

  void Close();

 private:
  int fd_ = -1;
  std::string read_buf_;
};

}
