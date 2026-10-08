#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "terminal/TerminalInput.h"

namespace microide::platform {
class ProcessLauncher;
}

namespace microide::terminal {

class TerminalSession;

// The client's end of one host terminal (dev-docs/design/remote-projects.md
// § 6.5): what a TerminalSession in host mode sends its input, size and close
// through. Frames come back the other way, into TerminalSession::ApplyHostFrame.
//
// Threading: callable from the UI thread; implementations only queue.
class TerminalHostChannel {
 public:
  virtual ~TerminalHostChannel() = default;
  virtual void Send(const TerminalInputEvent& event) = 0;
  virtual void Resize(std::size_t rows, std::size_t columns) = 0;
  // Close the terminal on the host and stop delivering frames. When this returns
  // no frame is being applied and none will be.
  virtual void Close() = 0;
};

// A launcher whose shells run on ANOTHER machine also implements this, the way
// such a launcher implements project::GitMetadataSource: TerminalSession::Start
// asks the launcher it is given, and a host terminal is a different backend
// rather than a different code path for every caller.
class HostTerminalSource {
 public:
  struct OpenRequest {
    std::filesystem::path working_directory;  // in the EDITOR's tree; the source maps it
    std::string command;
    std::vector<std::string> shell;
    std::size_t rows = 24;
    std::size_t columns = 80;
    std::size_t scrollback_lines = 2000;
  };

  virtual ~HostTerminalSource() = default;
  // Open a host terminal whose frames are applied to `session` until the
  // returned channel is closed. Null with *error when the host refused or is gone.
  virtual std::shared_ptr<TerminalHostChannel> OpenTerminal(const OpenRequest& request,
                                                            TerminalSession& session,
                                                            std::string* error) const = 0;
};

// The source for terminals started through `launcher`, or null when its shells
// run on this machine.
const HostTerminalSource* HostTerminalsFor(const platform::ProcessLauncher& launcher);

}  // namespace microide::terminal
