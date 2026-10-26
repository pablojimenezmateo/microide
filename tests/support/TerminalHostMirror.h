#pragma once

// A host terminal and a client mirroring it, with the real frame builder, encoder
// and decoder between them and a channel that answers term/scrollback from the
// host -- the server and the link without the server or the link. Shared by the
// kernel's TerminalHost tests and the shell tests that need a host terminal in a
// pane (an external client session).

#include "TestSupport.h"
#include "TerminalSessionTestAccess.h"

#include "terminal/TerminalHostFrameBuilder.h"
#include "terminal/TerminalHostWire.h"
#include "terminal/TerminalSession.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace microide::tests {

inline std::string LineText(const terminal::TerminalLine& line) {
  std::string text;
  for (const auto& cell : line.cells) {
    text += cell.DisplayText().empty() ? std::string(" ") : std::string(cell.DisplayText());
  }
  while (!text.empty() && text.back() == ' ') {
    text.pop_back();
  }
  return text;
}

class RecordingChannel final : public terminal::TerminalHostChannel {
 public:
  void Send(const terminal::TerminalInputEvent& event) override { events.push_back(event); }
  void Resize(std::size_t rows, std::size_t columns) override {
    resized_rows = rows;
    resized_columns = columns;
  }
  void Close() override { closed = true; }
  // term/scrollback, answered synchronously from the host session, as the server
  // does (CaptureHostHistory); unset, nothing is sent.
  bool FetchHistory(std::uint64_t before, std::size_t count, HistoryDone done) override {
    if (history_source == nullptr) {
      return false;
    }
    ++history_requests;
    std::vector<terminal::TerminalLine> lines;
    const std::uint64_t first = history_source->CaptureHostHistory(before, count, lines);
    done(first, std::move(lines));
    return true;
  }

  const terminal::TerminalSession* history_source = nullptr;
  std::size_t history_requests = 0;
  std::vector<terminal::TerminalInputEvent> events;
  std::size_t resized_rows = 0;
  std::size_t resized_columns = 0;
  bool closed = false;
};

// A host session, a client session in host mode, and the builder between them;
// every Sync goes through the real encoder and decoder.
struct Mirror {
  terminal::TerminalSession host;
  terminal::TerminalSession own_client;
  // `own_client`, or a session the test owns elsewhere (a shell's terminal pane).
  terminal::TerminalSession& client;
  std::shared_ptr<RecordingChannel> channel = std::make_shared<RecordingChannel>();
  terminal::TerminalHostFrameBuilder builder;
  terminal::TerminalSession::HostCapture capture;
  terminal::TerminalHostFrame frame;
  std::size_t last_frame_bytes = 0;

  Mirror(std::size_t rows, std::size_t columns) : Mirror(own_client, rows, columns) {}
  Mirror(terminal::TerminalSession& external_client, std::size_t rows, std::size_t columns)
      : client(external_client) {
    TerminalSessionTestAccess::Reset(host, rows, columns);
    TerminalSessionTestAccess::Reset(client, rows, columns);
    TerminalSessionTestAccess::SetRunning(host, true);
    TerminalSessionTestAccess::EnterHostMode(client, channel);
  }
  ~Mirror() { client.Stop(); }

  void Write(std::string_view bytes) { TerminalSessionTestAccess::AppendOutput(host, bytes); }

  bool Sync(std::size_t budget = 1u << 30, std::uint64_t echo_ack = 0) {
    host.CaptureForHost(builder.capture_from(), builder.capture_lines_before_screen(500), capture);
    builder.Build(capture, budget, frame);
    frame.echo_ack = echo_ack;
    std::string wire;
    terminal::EncodeTerminalHostFrame(wire, frame);
    last_frame_bytes = wire.size();
    std::optional<terminal::TerminalHostFrame> decoded = terminal::DecodeTerminalHostFrame(wire);
    Expect(decoded.has_value(), "a built frame decodes");
    return decoded.has_value() && client.ApplyHostFrame(std::move(*decoded));
  }

  // The client shows exactly what the host does: every line, the cursor and the modes.
  void ExpectSame(const std::string& what) {
    const std::vector<terminal::TerminalLine> host_lines = host.SnapshotLines();
    const std::vector<terminal::TerminalLine> client_lines = client.SnapshotLines();
    Expect(host_lines.size() == client_lines.size(),
           what + ": line count " + std::to_string(client_lines.size()) + " vs host " +
               std::to_string(host_lines.size()));
    for (std::size_t i = 0; i < std::min(host_lines.size(), client_lines.size()); ++i) {
      Expect(terminal::SameLine(host_lines[i], client_lines[i]),
             what + ": line " + std::to_string(i) + " is '" + LineText(client_lines[i]) +
                 "', host '" + LineText(host_lines[i]) + "'");
    }
    Expect(host.cursor_row() == client.cursor_row() && host.cursor_column() == client.cursor_column(),
           what + ": cursor " + std::to_string(client.cursor_row()) + "," +
               std::to_string(client.cursor_column()) + " vs host " +
               std::to_string(host.cursor_row()) + "," + std::to_string(host.cursor_column()));
    Expect(host.using_alternate_screen() == client.using_alternate_screen(),
           what + ": the same screen");
  }
};

}  // namespace microide::tests
