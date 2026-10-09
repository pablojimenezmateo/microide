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
  // Bounded so a client cannot ask the host to buffer without limit.
  request.credit_bytes = static_cast<std::size_t>(
      std::clamp<std::int64_t>(params["credit_bytes"].AsInt(0), 0, 16 * 1024 * 1024));
  request.prefetch_lines = static_cast<std::size_t>(
      std::clamp<std::int64_t>(params["prefetch_lines"].AsInt(0), 0, 100000));
  request.rows = static_cast<std::size_t>(rows);
  request.columns = static_cast<std::size_t>(columns);
  request.scrollback_lines = static_cast<std::size_t>(scrollback);
  return request;
}

TerminalTable::Client* TerminalTable::Terminal::FindClient(std::uint64_t connection) {
  for (Client& client : clients) {
    if (client.connection == connection) {
      return &client;
    }
  }
  return nullptr;
}

void TerminalTable::ApplySharedSizeLocked(Terminal& terminal) {
  std::size_t rows = 0;
  std::size_t columns = 0;
  for (const Client& client : terminal.clients) {
    if (client.rows == 0 || client.columns == 0) {
      continue;
    }
    rows = rows == 0 ? client.rows : std::min(rows, client.rows);
    columns = columns == 0 ? client.columns : std::min(columns, client.columns);
  }
  // Nobody attached, or nobody has said: keep the size it has.
  if (rows == 0 || (rows == terminal.session.rows() && columns == terminal.session.columns())) {
    return;
  }
  terminal.session.Resize(rows, columns);
  terminal.output_generation.fetch_add(1, std::memory_order_acq_rel);
}

TerminalTable::OpenResult TerminalTable::Open(std::uint64_t connection, OpenRequest request) {
  auto terminal = std::make_shared<Terminal>();
  {
    std::lock_guard lock(mutex_);
    if (terminals_.size() >= limits_.max_terminals) {
      return OpenResult{.error = "too many terminals on this host"};
    }
    terminal->handle = next_handle_++;
    terminal->credit_bytes =
        request.credit_bytes != 0 ? std::max<std::size_t>(request.credit_bytes, 16 * 1024)
                                  : limits_.credit_bytes;
    terminal->prefetch_lines =
        request.prefetch_lines != 0 ? request.prefetch_lines : limits_.prefetch_lines;
    terminal->clients.push_back(
        Client{.connection = connection, .rows = request.rows, .columns = request.columns});
  }
  Terminal* raw = terminal.get();
  terminal->session.SetOutputObserver([this, raw]() {
    raw->output_generation.fetch_add(1, std::memory_order_acq_rel);
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
  const std::size_t credit = terminal->credit_bytes;
  {
    std::lock_guard lock(mutex_);
    terminals_.emplace(handle, std::move(terminal));
  }
  Notify();
  return OpenResult{.handle = handle, .credit_bytes = credit};
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
    // The same connection attaching again (a reattach over it) starts over; any
    // other connection is one more client, sharing the terminal.
    Client* client = terminal.FindClient(connection);
    if (client == nullptr) {
      client = &terminal.clients.emplace_back();
      client->connection = connection;
    } else {
      // Input sequence numbers and credit are per client: a new start resets them.
      *client = Client{.connection = connection, .rows = client->rows, .columns = client->columns};
    }
    if (resume.has_value()) {
      client->builder.Resume(resume->screen_top, resume->alternate);
    } else {
      client->builder.Reset();
    }
    client->seen_generation = 0;  // due at once
  }
  Notify();
  return true;
}

void TerminalTable::Input(std::uint64_t connection, std::uint64_t handle,
                          const std::vector<terminal::TerminalInputEvent>& events) {
  std::shared_ptr<Terminal> terminal;
  {
    std::lock_guard lock(mutex_);
    const auto it = terminals_.find(handle);
    // Only an attached client types into a terminal.
    if (it == terminals_.end() || events.empty() || it->second->FindClient(connection) == nullptr) {
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
    if (Client* client = terminal->FindClient(connection)) {
      client->written_seq = std::max(client->written_seq, events.back().seq);
      client->written_at = std::chrono::steady_clock::now();
      client->written_generation = terminal->output_generation.load(std::memory_order_acquire);
    }
  }
  Notify();
}

void TerminalTable::Resize(std::uint64_t connection, std::uint64_t handle, std::size_t rows,
                           std::size_t columns) {
  {
    std::lock_guard lock(mutex_);
    const auto it = terminals_.find(handle);
    if (it == terminals_.end()) {
      return;
    }
    Client* client = it->second->FindClient(connection);
    if (client == nullptr) {
      return;
    }
    client->rows = std::clamp<std::size_t>(rows, 1, terminal::kMaxHostTerminalDimension);
    client->columns = std::clamp<std::size_t>(columns, 1, terminal::kMaxHostTerminalDimension);
    ApplySharedSizeLocked(*it->second);
  }
  Notify();
}

void TerminalTable::Ack(std::uint64_t connection, std::uint64_t handle,
                        std::uint64_t received_bytes) {
  {
    std::lock_guard lock(mutex_);
    const auto it = terminals_.find(handle);
    if (it == terminals_.end()) {
      return;
    }
    Client* client = it->second->FindClient(connection);
    if (client == nullptr) {
      return;
    }
    // Never past what was sent, never backwards: an ack is untrusted.
    client->acked_bytes = std::clamp(received_bytes, client->acked_bytes, client->sent_bytes);
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
    const auto removed = std::erase_if(terminal->clients, [connection](const Client& client) {
      return client.connection == connection;
    });
    if (removed != 0) {
      ApplySharedSizeLocked(*terminal);  // the others may have room again
    }
  }
}

std::size_t TerminalTable::AttachedCount(std::uint64_t handle) const {
  std::lock_guard lock(mutex_);
  const auto it = terminals_.find(handle);
  return it == terminals_.end() ? 0 : it->second->clients.size();
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
  struct Due {
    std::shared_ptr<Terminal> terminal;
    std::uint64_t connection = 0;
  };
  std::vector<Outgoing> outgoing;
  std::vector<Due> due;
  terminal::TerminalSession::HostCapture capture;
  terminal::TerminalHostFrame frame;
  std::unique_lock lock(mutex_);
  while (!stop_) {
    const Clock::time_point now = Clock::now();
    std::optional<Clock::time_point> next_wake;
    const auto wake_at = [&](Clock::time_point when) {
      next_wake = next_wake ? std::min(*next_wake, when) : when;
    };
    const auto echo_due = [&](const Terminal& terminal, const Client& client) {
      const bool answered =
          terminal.output_generation.load(std::memory_order_acquire) > client.written_generation;
      return client.written_seq > client.echo_ack &&
             (answered || now - client.written_at >= limits_.echo_timeout);
    };
    due.clear();
    for (auto& [handle, terminal] : terminals_) {
      (void)handle;
      const std::uint64_t generation = terminal->output_generation.load(std::memory_order_acquire);
      for (Client& client : terminal->clients) {
        const bool echo = echo_due(*terminal, client);
        if (client.written_seq > client.echo_ack && !echo) {
          wake_at(client.written_at + limits_.echo_timeout);
        }
        if (client.seen_generation == generation && !echo && !client.signal_pending()) {
          continue;
        }
        const std::uint64_t in_flight = client.sent_bytes - client.acked_bytes;
        const auto interval = in_flight > terminal->credit_bytes / 2 ? limits_.congested_interval
                                                                   : limits_.frame_interval;
        if (now - client.last_sent < interval) {
          wake_at(client.last_sent + interval);
          continue;
        }
        due.push_back(Due{.terminal = terminal, .connection = client.connection});
      }
    }

    for (const Due& item : due) {
      Terminal& terminal = *item.terminal;
      Client* client = terminal.FindClient(item.connection);
      if (client == nullptr) {
        continue;
      }
      // Read the echo state BEFORE capturing: output that answered the input is
      // then already in the capture, so the ack never claims what it cannot show.
      const std::uint64_t echo_ack =
          echo_due(terminal, *client) ? client->written_seq : client->echo_ack;
      client->seen_generation = terminal.output_generation.load(std::memory_order_acquire);
      terminal.session.CaptureForHost(
          client->builder.capture_from(),
          client->builder.capture_lines_before_screen(terminal.prefetch_lines), capture);
      // A capture CONSUMES the one-shot signals; every client is owed them.
      terminal::TerminalHostFrame& header = capture.header;
      if (header.has(terminal::TerminalHostFrame::kBell) || header.clipboard || header.notification) {
        for (Client& other : terminal.clients) {
          other.bell = other.bell || header.has(terminal::TerminalHostFrame::kBell);
          if (header.clipboard) {
            other.clipboard = header.clipboard;
          }
          if (header.notification) {
            other.notification = header.notification;
          }
        }
        header.flags &= static_cast<std::uint16_t>(~terminal::TerminalHostFrame::kBell);
        header.clipboard.reset();
        header.notification.reset();
      }
      if (echo_ack == client->echo_ack && !client->signal_pending() &&
          client->builder.UpToDate(capture)) {
        continue;
      }
      const std::uint64_t in_flight = client->sent_bytes - client->acked_bytes;
      const std::size_t budget = in_flight >= terminal.credit_bytes
                                     ? 0
                                     : terminal.credit_bytes - static_cast<std::size_t>(in_flight);
      client->builder.Build(capture, budget, frame);
      frame.echo_ack = echo_ack;
      if (client->bell) {
        frame.flags |= terminal::TerminalHostFrame::kBell;
      }
      frame.clipboard = std::move(client->clipboard);
      frame.notification = std::move(client->notification);
      client->bell = false;
      client->clipboard.reset();
      client->notification.reset();
      client->echo_ack = echo_ack;
      Outgoing out{.connection = client->connection, .handle = terminal.handle};
      terminal::EncodeTerminalHostFrame(out.wire, frame);
      client->sent_bytes += out.wire.size();
      client->last_sent = now;
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
