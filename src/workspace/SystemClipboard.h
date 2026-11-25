#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace microide::workspace {

class SystemClipboard {
 public:
  struct Backend {
    std::function<bool(std::string_view)> set;
    std::function<std::optional<std::string>()> get;
    std::function<std::optional<std::string>()> get_png;
  };

  SystemClipboard();
  explicit SystemClipboard(Backend backend) : backend_(std::move(backend)) {}

  void SetReaderForTesting(std::function<std::optional<std::string>()> get) {
    backend_.get = std::move(get);
  }
  void SetWriterForTesting(std::function<bool(std::string_view)> set) {
    backend_.set = std::move(set);
  }
  void SetPngReaderForTesting(std::function<std::optional<std::string>()> get_png) {
    backend_.get_png = std::move(get_png);
  }

  bool Write(std::string_view text);
  std::optional<std::string> Read();
  std::optional<std::string> ReadPng();
  static constexpr std::size_t kMaxPngBytes = 64u * 1024 * 1024;

 private:
  Backend backend_;
  std::optional<std::string> held_;
  std::optional<std::string> system_at_hold_;
};

}
