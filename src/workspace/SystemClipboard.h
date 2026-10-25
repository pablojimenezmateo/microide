#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace microide::workspace {

// The system clipboard, with the app's own copy kept when the system refuses it.
//
// A Wayland compositor accepts a clipboard selection only with the serial of a
// recent input event on the client. Input the control channel delivers carries
// none, so an agent's `copy` was refused and its `paste` then inserted nothing
// (TD-2026-10-19-366). A copy the system does not hold afterwards -- read back at
// once -- is held here and answers the app's own paste for as long as the system
// clipboard still holds what it held at that moment; anything another client
// copies since takes over, as it would have anyway. Other clients still cannot
// see a refused copy: nothing in this process can make the compositor take it.
class SystemClipboard {
 public:
  struct Backend {
    std::function<bool(std::string_view)> set;
    std::function<std::optional<std::string>()> get;
  };

  // The SDL clipboard.
  SystemClipboard();
  explicit SystemClipboard(Backend backend) : backend_(std::move(backend)) {}

  // Tests replace one half of the backend at a time.
  void SetReaderForTesting(std::function<std::optional<std::string>()> get) {
    backend_.get = std::move(get);
  }
  void SetWriterForTesting(std::function<bool(std::string_view)> set) {
    backend_.set = std::move(set);
  }

  // True when the text is on the clipboard -- the system's, or held here for the
  // app's own paste.
  bool Write(std::string_view text);
  std::optional<std::string> Read();

 private:
  Backend backend_;
  std::optional<std::string> held_;            // a copy the system refused
  std::optional<std::string> system_at_hold_;  // what the system held when it did
};

}  // namespace microide::workspace
