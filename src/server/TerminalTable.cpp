#include "server/TerminalTable.h"

#include <algorithm>
#include <cstdlib>
#include <utility>

#include "platform/ProcessLauncher.h"
#include "util/Log.h"

namespace microide::server {
namespace {

constexpr std::size_t kMaxShellArgs = 256;
constexpr std::size_t kMaxShellBytes = 64 * 1024;

}  // namespace

TerminalTable::TerminalTable(Sink sink, Limits limits) : sink_(std::move(sink)), limits_(limits) {
  thread_ = std::thread([this]() { Run(); });
}

TerminalTable::~TerminalTable() {
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
  }
  wake_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
  std::map<std::uint64_t, std::shared_ptr<Terminal>> terminals;
  {
    std::lock_guard lock(mutex_);
    terminals.swap(terminals_);
  }
  for (auto& [handle, terminal] : terminals) {
    (void)handle;
    terminal->session.Stop();
  }
}

std::optional<TerminalTable::OpenRequest> TerminalTable::ParseOpen(const util::JsonValue& params,
                                                                   std::string* error) {
  const auto fail = [&](std::string message) -> std::optional<OpenRequest> {
    if (error != nullptr) {
      *error = std::move(message);
    }
    return std::nullopt;
  };
  if (!params.IsObject()) {
    return fail("term/open params must be an object");
  }
  OpenRequest request;
  // No cwd: the user's home, as a login shell would start (a host terminal opened
  // outside any project).
  const util::JsonValue& cwd = params["cwd"];
  if (cwd.IsNull()) {
    const char* home = std::getenv("HOME");
    request.cwd = home != nullptr && home[0] == '/' ? home : "/";
  } else if (!cwd.IsString() || cwd.AsString().empty() || cwd.AsString().front() != '/') {
    return fail("cwd must be an absolute path");
  } else {
    request.cwd = cwd.AsString();
  }
  if (const util::JsonValue& command = params["command"]; !command.IsNull()) {
    if (!command.IsString() || command.AsString().size() > kMaxShellBytes) {
      return fail("command must be a string");
    }
    request.command = command.AsString();
  }
  if (const util::JsonValue& shell = params["shell"]; !shell.IsNull()) {
    if (!shell.IsArray() || shell.AsArray().size() > kMaxShellArgs) {
      return fail("shell must be an argv array");
    }
    std::size_t bytes = 0;
    for (const util::JsonValue& arg : shell.AsArray()) {
      if (!arg.IsString() || arg.AsString().find('\0') != std::string::npos) {
        return fail("shell arguments must be strings without NUL");
      }
      bytes += arg.AsString().size();
      request.shell.push_back(arg.AsString());
    }
    if (bytes > kMaxShellBytes) {
      return fail("shell argv too large");
    }
  }
  const std::int64_t rows = params["rows"].AsInt(24);
  const std::int64_t columns = params["columns"].AsInt(80);
  const std::int64_t scrollback = params["scrollback_lines"].AsInt(2000);
  const auto in_range = [](std::int64_t value, std::int64_t max) { return value >= 1 && value <= max; };
  if (!in_range(rows, terminal::kMaxHostTerminalDimension) ||
      !in_range(columns, terminal::kMaxHostTerminalDimension) || !in_range(scrollback, 1000000)) {
    return fail("rows, columns and scrollback_lines must be positive and bounded");
  }
  request.rows = static_cast<std::size_t>(rows);
  request.columns = static_cast<std::size_t>(columns);
  request.scrollback_lines = static_cast<std::size_t>(scrollback);
  return request;
}

TerminalTable::OpenResult TerminalTable::Open(std::uint64_t connection, OpenRequest request) {
  auto terminal = std::make_shared<Terminal>();
  {
    std::lock_guard lock(mutex_);
    if (terminals_.size() >= limits_.max_terminals) {
      return OpenResult{.error = "too many terminals on this host"};
    }
    terminal->handle = next_handle_++;
    terminal->connection = connection;
  }
  Terminal* raw = terminal.get();
  terminal->session.SetOutputObserver([this, raw]() {
    raw->dirty.store(true, std::memory_order_release);
    raw->output_since_write.store(true, std::memory_order_release);
    Notify();
  });
  terminal->session.SetMaxScrollbackLines(request.scrollback_lines);
  // This process IS the host: the shell runs here, through the local launcher.
  if (!terminal->session.StartArgv(platform::LocalProcessLauncher(), request.cwd, request.command,
                                   std::move(request.shell))) {
    terminal->session.Stop();
    return OpenResult{.error = "could not start a shell in " + request.cwd};
  }
  terminal->session.Resize(request.rows, request.columns);
  const std::uint64_t handle = terminal->handle;
  {
    std::lock_guard lock(mutex_);
    terminals_.emplace(handle, std::move(terminal));
  }
  Notify();
  return OpenResult{.handle = handle};
}

bool TerminalTable::Attach(std::uint64_t connection, std::uint64_t handle,
                           std::optional<Resume> resume) {
  {
    std::lock_guard lock(mutex_);
    const auto it = terminals_.find(handle);
    if (it == terminals_.end()) {
      return false;
    }
    Terminal& terminal = *it->second;
    terminal.connection = connection;
    if (resume.has_value()) {
      terminal.builder.Resume(resume->screen_top, resume->alternate);
    } else {
      terminal.builder.Reset();
    }
    // Input sequence numbers are per client: a new client starts its own.
    terminal.written_seq = 0;
    terminal.echo_ack = 0;
    terminal.sent_bytes = 0;
    terminal.acked_bytes = 0;
    terminal.last_sent = {};
    terminal.dirty.store(true, std::memory_order_release);
  }
  Notify();
  return true;
}

void TerminalTable::Input(std::uint64_t handle,
                          const std::vector<terminal::TerminalInputEvent>& events) {
  std::shared_ptr<Terminal> terminal;
  {
    std::lock_guard lock(mutex_);
    const auto it = terminals_.find(handle);
    if (it == terminals_.end() || events.empty()) {
      return;
    }
    terminal = it->second;
  }
  using Kind = terminal::TerminalInputEvent::Kind;
  terminal::TerminalSession& session = terminal->session;
  // Encoded HERE, against the modes this session is in now — not the client's
  // one-round-trip-old view of them.
  for (const terminal::TerminalInputEvent& event : events) {
    switch (event.kind) {
      case Kind::Key:
        (void)session.SendKeyPress(event.key);
        break;
      case Kind::Paste:
        session.PasteText(event.text);
        break;
      case Kind::Bytes:
        session.SendBytes(event.text);
        break;
      case Kind::MouseButton:
        (void)session.SendMouseButton(event.button, event.pressed, event.row, event.column,
                                      event.modifiers);
        break;
      case Kind::MouseMotion:
        (void)session.SendMouseMotion(event.button, event.row, event.column, event.modifiers);
        break;
      case Kind::Focus:
        session.SendFocusEvent(event.pressed);
        break;
    }
  }
  {
    std::lock_guard lock(mutex_);
    terminal->written_seq = std::max(terminal->written_seq, events.back().seq);
    terminal->written_at = std::chrono::steady_clock::now();
    terminal->output_since_write.store(false, std::memory_order_release);
  }
  Notify();
}

void TerminalTable::Resize(std::uint64_t handle, std::size_t rows, std::size_t columns) {
  std::shared_ptr<Terminal> terminal;
  {
    std::lock_guard lock(mutex_);
    const auto it = terminals_.find(handle);
    if (it == terminals_.end()) {
      return;
    }
    terminal = it->second;
  }
  terminal->session.Resize(std::clamp<std::size_t>(rows, 1, terminal::kMaxHostTerminalDimension),
                           std::clamp<std::size_t>(columns, 1, terminal::kMaxHostTerminalDimension));
  terminal->dirty.store(true, std::memory_order_release);
  Notify();
}

void TerminalTable::Ack(std::uint64_t handle, std::uint64_t received_bytes) {
  {
    std::lock_guard lock(mutex_);
    const auto it = terminals_.find(handle);
    if (it == terminals_.end()) {
      return;
    }
    Terminal& terminal = *it->second;
    // Never past what was sent, never backwards: an ack is untrusted.
    terminal.acked_bytes =
        std::clamp(received_bytes, terminal.acked_bytes, terminal.sent_bytes);
  }
  Notify();
}

void TerminalTable::Close(std::uint64_t handle) {
  std::shared_ptr<Terminal> terminal;
  {
    std::lock_guard lock(mutex_);
    const auto it = terminals_.find(handle);
    if (it == terminals_.end()) {
      return;
    }
    terminal = std::move(it->second);
    terminals_.erase(it);
  }
  terminal->session.Stop();
}

void TerminalTable::Detach(std::uint64_t connection) {
  std::lock_guard lock(mutex_);
  for (auto& [handle, terminal] : terminals_) {
    (void)handle;
    if (terminal->connection == connection) {
      terminal->connection = 0;
    }
  }
}

std::size_t TerminalTable::LiveCount() const {
  std::lock_guard lock(mutex_);
  return terminals_.size();
}

void TerminalTable::Notify() {
  {
    std::lock_guard lock(mutex_);
    woken_ = true;
  }
  wake_.notify_one();
}

void TerminalTable::Run() {
  using Clock = std::chrono::steady_clock;
  struct Outgoing {
    std::uint64_t connection = 0;
    std::uint64_t handle = 0;
    std::string wire;
  };
  std::vector<Outgoing> outgoing;
  std::vector<std::shared_ptr<Terminal>> due;
  terminal::TerminalSession::HostCapture capture;
  terminal::TerminalHostFrame frame;
  std::unique_lock lock(mutex_);
  while (!stop_) {
    const Clock::time_point now = Clock::now();
    std::optional<Clock::time_point> next_wake;
    const auto wake_at = [&](Clock::time_point when) {
      next_wake = next_wake ? std::min(*next_wake, when) : when;
    };
    due.clear();
    for (auto& [handle, terminal] : terminals_) {
      (void)handle;
      if (terminal->connection == 0) {
        continue;
      }
      const bool answered = terminal->output_since_write.load(std::memory_order_acquire);
      const bool echo_due = terminal->written_seq > terminal->echo_ack &&
                            (answered || now - terminal->written_at >= limits_.echo_timeout);
      if (terminal->written_seq > terminal->echo_ack && !echo_due) {
        wake_at(terminal->written_at + limits_.echo_timeout);
      }
      if (!terminal->dirty.load(std::memory_order_acquire) && !echo_due) {
        continue;
      }
      const std::uint64_t in_flight = terminal->sent_bytes - terminal->acked_bytes;
      const auto interval = in_flight > limits_.credit_bytes / 2 ? limits_.congested_interval
                                                                 : limits_.frame_interval;
      if (now - terminal->last_sent < interval) {
        wake_at(terminal->last_sent + interval);
        continue;
      }
      due.push_back(terminal);
    }

    for (const std::shared_ptr<Terminal>& terminal : due) {
      // Read the echo state BEFORE capturing: output that answered the input is
      // then already in the capture, so the ack never claims what it cannot show.
      const bool answered = terminal->output_since_write.load(std::memory_order_acquire);
      const std::uint64_t echo_ack =
          terminal->written_seq > terminal->echo_ack &&
                  (answered || now - terminal->written_at >= limits_.echo_timeout)
              ? terminal->written_seq
              : terminal->echo_ack;
      terminal->dirty.store(false, std::memory_order_release);
      terminal->session.CaptureForHost(
          terminal->builder.capture_from(),
          terminal->builder.capture_lines_before_screen(limits_.prefetch_lines), capture);
      if (echo_ack == terminal->echo_ack && terminal->builder.UpToDate(capture)) {
        continue;
      }
      const std::uint64_t in_flight = terminal->sent_bytes - terminal->acked_bytes;
      const std::size_t budget =
          in_flight >= limits_.credit_bytes ? 0 : limits_.credit_bytes - static_cast<std::size_t>(in_flight);
      terminal->builder.Build(capture, budget, frame);
      frame.echo_ack = echo_ack;
      terminal->echo_ack = echo_ack;
      Outgoing out{.connection = terminal->connection, .handle = terminal->handle};
      terminal::EncodeTerminalHostFrame(out.wire, frame);
      terminal->sent_bytes += out.wire.size();
      terminal->last_sent = now;
      outgoing.push_back(std::move(out));
    }

    if (!outgoing.empty()) {
      lock.unlock();
      for (const Outgoing& out : outgoing) {
        sink_.frame(out.connection, out.handle, out.wire);
      }
      outgoing.clear();
      lock.lock();
      continue;  // re-evaluate: input may have arrived while sending
    }
    if (woken_) {
      woken_ = false;
      continue;
    }
    if (next_wake) {
      wake_.wait_until(lock, *next_wake, [this] { return stop_ || woken_; });
    } else {
      wake_.wait(lock, [this] { return stop_ || woken_; });
    }
    woken_ = false;
  }
}

}  // namespace microide::server
