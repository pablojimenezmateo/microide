#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "terminal/TerminalHostFrameBuilder.h"
#include "terminal/TerminalHostWire.h"
#include "terminal/TerminalSession.h"
#include "util/JsonValue.h"

namespace microide::server {

// The host's terminals (dev-docs/design/remote-projects.md § 6.5, `term/*`): each
// is a TerminalSession running here, in a pty the server owns, so a dropped link
// signals nothing in it. The client receives frames (TerminalHostWire.h), never
// the byte stream, and sends semantic input this table encodes against the
// session's own modes.
//
// Frames are coalesced: at most one per 16 ms per terminal, one per 33 ms while
// more than half the client's credit window is in flight. A frame also goes out
// when only `echo_ack` moved — once the pty has answered the latest input, or
// 50 ms after it was written with no answer (a password prompt), so the client's
// prediction is confirmed or contradicted within a round trip either way.
//
// A terminal outlives its connection: Detach stops its frames, Attach resumes them
// with a cold frame (screen plus prefetched history), and it ends only on Close —
// so a shell that exited while nobody was attached keeps its output until seen.
//
// Several clients may attach to one terminal at once (a second machine, a second
// window): each has its own frame builder, credit window, input sequence and
// one-shot signals (bell, clipboard, notification), and the pty is sized to the
// SMALLEST of them — tmux's rule (spec "Two clients share one pty at the smaller
// size"), so neither sees a program draw past its edge.
class TerminalTable {
 public:
  struct Sink {
    // A frame for terminal `handle` to connection `connection`; called with no
    // table lock held.
    std::function<void(std::uint64_t connection, std::uint64_t handle, std::string_view frame)>
        frame;
  };

  struct Limits {
    std::size_t credit_bytes = 256 * 1024;  // frame bytes in flight per terminal
    std::size_t prefetch_lines = 500;       // history in an attach frame
    std::size_t max_terminals = 64;
    std::chrono::milliseconds frame_interval{16};
    std::chrono::milliseconds congested_interval{33};
    std::chrono::milliseconds echo_timeout{50};
  };

  struct OpenRequest {
    std::string cwd;  // a host path
    std::string command;
    std::vector<std::string> shell;  // argv; empty = the login shell
    std::size_t rows = 24;
    std::size_t columns = 80;
    std::size_t scrollback_lines = 2000;
    // The client's preferences (remote.term_credit_bytes,
    // remote.scrollback_prefetch_lines), clamped; 0 = the table's default.
    std::size_t credit_bytes = 0;
    std::size_t prefetch_lines = 0;
  };
  struct OpenResult {
    std::uint64_t handle = 0;
    std::size_t credit_bytes = 0;  // what the terminal was given
    std::string error;
  };

  TerminalTable(Sink sink, Limits limits);
  explicit TerminalTable(Sink sink) : TerminalTable(std::move(sink), Limits{}) {}
  ~TerminalTable();
  TerminalTable(const TerminalTable&) = delete;
  TerminalTable& operator=(const TerminalTable&) = delete;

  // Decode and validate a term/open body (untrusted): nullopt with *error.
  static std::optional<OpenRequest> ParseOpen(const util::JsonValue& params, std::string* error);

  OpenResult Open(std::uint64_t connection, OpenRequest request);
  // Take over terminal `handle` (a reattach). Cold (`resume` unset): the next
  // frame is the screen plus prefetched history. Warm: the client still holds
  // everything through its screen at `resume->screen_top`, and gets only what
  // came after.
  struct Resume {
    std::uint64_t screen_top = 0;
    bool alternate = false;
  };
  bool Attach(std::uint64_t connection, std::uint64_t handle, std::optional<Resume> resume = {});
  void Input(std::uint64_t connection, std::uint64_t handle,
             const std::vector<terminal::TerminalInputEvent>& events);
  // `connection`'s pane size; the pty takes the smallest of the attached clients'.
  void Resize(std::uint64_t connection, std::uint64_t handle, std::size_t rows, std::size_t columns);
  // `connection` has received this many frame bytes of this terminal in total.
  void Ack(std::uint64_t connection, std::uint64_t handle, std::uint64_t received_bytes);
  void Close(std::uint64_t handle);
  // A connection went away: its terminals keep running, without it.
  void Detach(std::uint64_t connection);
  // Clients attached to `handle` (for tests and status).
  std::size_t AttachedCount(std::uint64_t handle) const;

  std::size_t LiveCount() const;
  const Limits& limits() const { return limits_; }

 private:
  // One attached client's view of a terminal. Guarded by the table mutex.
  struct Client {
    std::uint64_t connection = 0;
    terminal::TerminalHostFrameBuilder builder;
    std::size_t rows = 0;  // its pane, 0 = not said yet
    std::size_t columns = 0;
    std::chrono::steady_clock::time_point last_sent{};
    std::uint64_t sent_bytes = 0;
    std::uint64_t acked_bytes = 0;
    std::uint64_t written_seq = 0;
    std::uint64_t echo_ack = 0;
    std::chrono::steady_clock::time_point written_at{};
    // The terminal's output_generation this client last captured, and the one
    // when it last wrote input: output after that answers it.
    std::uint64_t seen_generation = 0;
    std::uint64_t written_generation = 0;
    // One-shot signals a capture took for every client; this one's are unsent.
    bool bell = false;
    std::optional<std::string> clipboard;
    std::optional<terminal::TerminalHostNotification> notification;
    bool signal_pending() const { return bell || clipboard || notification; }
  };

  struct Terminal {
    std::uint64_t handle = 0;
    terminal::TerminalSession session;
    std::size_t credit_bytes = 0;
    std::size_t prefetch_lines = 0;
    std::vector<Client> clients;  // guarded by the table mutex
    // Bumped by the session's reader thread on every parsed chunk; never 0.
    std::atomic<std::uint64_t> output_generation{1};

    Client* FindClient(std::uint64_t connection);
  };

  // The pty takes the smallest pane any attached client reported. Table lock held.
  void ApplySharedSizeLocked(Terminal& terminal);
  void Run();
  void Notify();

  Sink sink_;
  Limits limits_;
  std::thread thread_;

  mutable std::mutex mutex_;
  std::condition_variable wake_;
  bool woken_ = false;
  bool stop_ = false;
  std::uint64_t next_handle_ = 1;
  std::map<std::uint64_t, std::shared_ptr<Terminal>> terminals_;
};

}  // namespace microide::server
