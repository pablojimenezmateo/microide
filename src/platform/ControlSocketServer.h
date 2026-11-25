#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "util/Waker.h"

namespace microide::platform {

struct ControlInboundMessage {
  std::uint64_t connection_id = 0;
  std::string line;
};

struct ControlRequestLineScan {
  std::vector<std::string> lines;
  std::size_t consumed_bytes = 0;
  bool oversize_line = false;
};

ControlRequestLineScan ScanControlRequestLines(std::string_view buffer,
                                               std::size_t max_line_bytes);

class ControlSocketServer {
 public:
  static constexpr std::size_t kMaxWriteBufferBytes = 8u << 20;
  static constexpr std::size_t kMaxLineBytes = kMaxWriteBufferBytes - 1;

  ControlSocketServer();
  ~ControlSocketServer();
  ControlSocketServer(const ControlSocketServer&) = delete;
  ControlSocketServer& operator=(const ControlSocketServer&) = delete;

  [[nodiscard]] bool Start(const std::filesystem::path& socket_path);
  bool IsRunning() const;
  void Stop();

  void SetWakeChannel(util::WakeChannel channel);

  std::vector<ControlInboundMessage> TakeInbound();
  void SendLine(std::uint64_t connection_id, const std::string& line);
  void Broadcast(const std::string& line);

  std::size_t ConnectionCount() const;

  bool ConsumeRebound();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}
