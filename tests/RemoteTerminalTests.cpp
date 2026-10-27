#include "TestSupport.h"
#include "RemoteServerTestSupport.h"
#include "TerminalSessionTestAccess.h"
#include "support/DelayRelay.h"

#include "platform/UnixSocket.h"
#include "project/remote/RemoteConnection.h"
#include "project/remote/RemoteProcessLauncher.h"
#include "project/remote/RemoteProtocol.h"
#include "project/remote/RemoteServerClient.h"
#include "project/remote/RemoteServerPaths.h"
#include "project/remote/RemoteTerminalChannel.h"
#include "terminal/TerminalSession.h"
#include "util/JsonValue.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

namespace microide::tests {
namespace {

namespace remote = project::remote;
using terminal::TerminalSession;

std::shared_ptr<remote::RemoteServerClient> Connect(const ShortServerDir& dir) {
  auto client = remote::RemoteServerClient::Create();
  const int fd = platform::ConnectUnixSocket(remote::ServerSocketPath(dir.path()));
  std::string error;
  Expect(fd >= 0 && client->ConnectFds(fd, fd, remote::HelloRequest{.release = "test"}, &error),
         "the client connects: " + error);
  return client;
}

std::shared_ptr<remote::RemoteServerClient> ConnectWithRoot(const ShortServerDir& dir,
                                                            const std::filesystem::path& root) {
  auto client = remote::RemoteServerClient::Create();
  const int fd = platform::ConnectUnixSocket(remote::ServerSocketPath(dir.path()));
  std::string error;
  Expect(fd >= 0 && client->ConnectFds(fd, fd,
                                       remote::HelloRequest{.release = "test", .root = root.string()},
                                       &error),
         "the client connects with a workspace: " + error);
  return client;
}

// The server's terminal listing, asked over a fresh connection of its own.
std::vector<remote::ServerTerminal> ListedTerminals(const ShortServerDir& dir) {
  auto client = Connect(dir);
  std::string error;
  const std::optional<remote::ServerStatus> status = client->Status(&error);
  Expect(status.has_value() && status->terminals.has_value(), "server/status lists terminals: " + error);
  return status.has_value() && status->terminals.has_value() ? *status->terminals
                                                             : std::vector<remote::ServerTerminal>{};
}

const remote::ServerTerminal* Listed(const std::vector<remote::ServerTerminal>& rows, std::uint64_t handle) {
  for (const remote::ServerTerminal& row : rows) {
    if (row.handle == handle) {
      return &row;
    }
  }
  return nullptr;
}

std::string ScreenText(const TerminalSession& session) {
  std::string text;
  for (const auto& line : session.SnapshotLines()) {
    for (const auto& cell : line.cells) {
      text += cell.DisplayText().empty() ? std::string(" ") : std::string(cell.DisplayText());
    }
    text += '\n';
  }
  return text;
}

bool WaitForText(const TerminalSession& session, std::string_view needle) {
  return WaitUntil([&] { return ScreenText(session).find(needle) != std::string::npos; },
                   std::chrono::seconds(15), std::chrono::milliseconds(5));
}

void TypeLine(TerminalSession& session, std::string_view text) {
  for (const char c : text) {
    TerminalSession::KeyPress press;
    press.key = TerminalSession::KeyPress::Key::Char;
    press.codepoint = static_cast<unsigned char>(c);
    (void)session.SendKeyPress(press);
  }
  TerminalSession::KeyPress enter;
  enter.key = TerminalSession::KeyPress::Key::Enter;
  (void)session.SendKeyPress(enter);
}

// A terminal started through a remote project's launcher runs its shell ON THE
// HOST, in the host's copy of the directory: typed keys reach it as semantic
// events, and what it prints comes back as frames. OSC 7 comes back mapped into
// the editor's tree, a title set with OSC 0 becomes the tab's label, and a resize
// reaches the host's pty.
void TestHostTerminalRunsTheShellOnTheHost() {
  ShortServerDir dir;
  StartServer(dir);
  const std::filesystem::path host = dir.scratch() / "host" / "project";
  const std::filesystem::path mirror = dir.scratch() / "mirror" / "project";
  std::filesystem::create_directories(host);
  std::filesystem::create_directories(mirror);
  auto client = Connect(dir);
  const remote::RemoteProcessLauncher launcher(client, remote::RemotePathMap(mirror, host),
                                               {.host_paths_readable_locally = true});
  TerminalSession session;
  Expect(session.Start(launcher, mirror, {}, "sh"), "the host terminal starts");
  Expect(session.is_host_terminal(), "a remote launcher's terminal is a host terminal");

  TypeLine(session, "echo HOST-$((40+2)) \"$PWD\"");
  Expect(WaitForText(session, "HOST-42 " + host.string()),
         "the shell runs on the host, in the host's tree:\n" + ScreenText(session));

  TypeLine(session, "printf '\\033]7;file://localhost%s\\a\\033]0;from-host\\a' \"$PWD\"");
  Expect(WaitUntil([&] { return session.LaunchLabel() == "from-host"; }, std::chrono::seconds(10)),
         "OSC 0 from the host becomes the label; got '" + session.LaunchLabel() + "'");
  Expect(WaitUntil([&] { return session.reported_working_directory() == mirror; },
                   std::chrono::seconds(10)),
         "OSC 7 names the host's directory and arrives as the mirror's: " +
             session.reported_working_directory().string());

  session.Resize(11, 53);
  TypeLine(session, "stty size");
  Expect(WaitForText(session, "11 53"), "the host's pty took the new size:\n" + ScreenText(session));
  Expect(session.rows() == 11 && session.columns() == 53, "the mirror follows the host's size");

  TypeLine(session, "exit");
  Expect(WaitUntil([&] { return !session.running(); }, std::chrono::seconds(10)),
         "the shell exiting on the host ends the terminal");
  session.Stop();
}

// The pty belongs to the server, not to the connection: a dropped link signals
// nothing in it. The shell keeps running and producing output, and a second
// connection that attaches to the same handle sees what it printed before AND
// while nobody was attached, and can keep typing into it.
void TestHostTerminalSurvivesADroppedConnection() {
  ShortServerDir dir;
  StartServer(dir);
  const std::filesystem::path root = dir.scratch() / "project";
  std::filesystem::create_directories(root);
  const auto identity = [](const std::filesystem::path& path) { return path; };

  std::uint64_t handle = 0;
  {
    auto first = Connect(dir);
    TerminalSession session;
    TerminalSessionTestAccess::Reset(session, 24, 80);
    auto channel = remote::RemoteTerminalChannel::Open(
        first, terminal::HostTerminalSource::OpenRequest{.working_directory = root, .shell = {"sh"}},
        session, identity);
    Expect(channel != nullptr, "the terminal opens");
    TerminalSessionTestAccess::EnterHostMode(session, channel);
    TypeLine(session, "echo before-$((1+1)); sleep 1; echo while-detached-$((2+3))");
    Expect(WaitForText(session, "before-2"), "output before the drop:\n" + ScreenText(session));
    handle = channel->handle();
    Expect(handle != 0, "the open was answered");
    first->peer().Fail("link dropped");  // nothing is closed on the host
    Expect(WaitUntil([&] { return !session.running(); }, std::chrono::seconds(5)),
           "the dropped link shows as an ended session");
    Expect(ScreenText(session).find("connection to the host lost") != std::string::npos,
           "and says why");
    TerminalSessionTestAccess::EnterHostMode(session, nullptr);
  }

  auto second = Connect(dir);
  TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);
  auto channel = remote::RemoteTerminalChannel::Attach(second, handle, session, identity);
  Expect(channel != nullptr, "the reattach is sent");
  TerminalSessionTestAccess::EnterHostMode(session, channel);
  Expect(WaitForText(session, "before-2"), "the reattached screen has the earlier output");
  Expect(WaitForText(session, "while-detached-5"),
         "and what the shell printed while detached:\n" + ScreenText(session));
  TypeLine(session, "echo after-$((3+4))");
  Expect(WaitForText(session, "after-7"), "and it takes input again");
  const std::string text = ScreenText(session);
  Expect(text.find("before-2") == text.rfind("before-2"),
         "nothing is duplicated by the reattach:\n" + text);
  session.Stop();
}

// Over a link with a real round trip (tests/support/DelayRelay.h): a typed
// character is drawn in the keystroke's own call, predicted, and the host's echo
// confirms it about one round trip later. With the server stalled the prediction
// stays pending -- nothing is invented in its place -- and it confirms once the
// stall clears. The relay itself is what the remote perf scenarios measure with.
void TestPredictionLeadsTheEchoByARoundTrip() {
  ShortServerDir dir;
  StartServer(dir);
  const std::filesystem::path root = dir.scratch() / "project";
  std::filesystem::create_directories(root);
  const auto identity = [](const std::filesystem::path& path) { return path; };
  constexpr auto kRtt = std::chrono::milliseconds(150);

  DelayRelay relay(platform::ConnectUnixSocket(remote::ServerSocketPath(dir.path())), kRtt);
  Expect(relay.ok(), "the relay is up");
  auto client = remote::RemoteServerClient::Create();
  const int fd = relay.TakeClientFd();
  std::string error;
  const auto hello_start = std::chrono::steady_clock::now();
  Expect(client->ConnectFds(fd, fd, remote::HelloRequest{.release = "test"}, &error),
         "the client connects through the relay: " + error);
  Expect(std::chrono::steady_clock::now() - hello_start >= kRtt * 9 / 10,
         "the hello took a round trip: the relay delays");

  TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 6, 60);
  session.SetPredictionMode(terminal::TerminalPredictionOverlay::Mode::Always);
  auto channel = remote::RemoteTerminalChannel::Open(
      client, terminal::HostTerminalSource::OpenRequest{.working_directory = root, .shell = {"sh"}},
      session, identity);
  Expect(channel != nullptr, "the terminal opens");
  TerminalSessionTestAccess::EnterHostMode(session, channel);
  // Split so the echoed command line cannot match the prompt it sets.
  TypeLine(session, "PS1='rea''dy> '");
  Expect(WaitForText(session, "ready> "), "the prompt arrives:\n" + ScreenText(session));

  const auto type = [&](char32_t c) {
    TerminalSession::KeyPress press;
    press.key = TerminalSession::KeyPress::Key::Char;
    press.codepoint = c;
    (void)session.SendKeyPress(press);
  };
  const auto typed_at = std::chrono::steady_clock::now();
  type(U'q');
  Expect(session.PendingPredictions() == 1 && ScreenText(session).find("ready> q") != std::string::npos,
         "the keystroke is drawn in its own call, before any round trip");
  Expect(WaitUntil([&] { return session.PendingPredictions() == 0; }, std::chrono::seconds(10),
                   std::chrono::milliseconds(2)),
         "the echo confirms it");
  const auto confirmed_after = std::chrono::steady_clock::now() - typed_at;
  Expect(confirmed_after >= kRtt * 9 / 10,
         "and the confirmation took the round trip it had to (" +
             std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(confirmed_after).count()) +
             " ms)");

  relay.SetServerStalled(true);
  type(U'w');
  std::this_thread::sleep_for(kRtt * 3);
  Expect(session.PendingPredictions() == 1, "a stalled server leaves the prediction pending");
  Expect(ScreenText(session).find("ready> qw") != std::string::npos,
         "and still drawn (" + TerminalSessionTestAccess::PredictionState(session) + "):\n" +
             ScreenText(session));
  relay.SetServerStalled(false);
  Expect(WaitUntil([&] { return session.PendingPredictions() == 0; }, std::chrono::seconds(10),
                   std::chrono::milliseconds(2)),
         "it confirms once the server answers again");
  Expect(ScreenText(session).find("ready> qw") != std::string::npos, "with the same text");
  session.Stop();
  channel.reset();
}

// A reconnect from the SAME session is warm: it keeps everything it already
// mirrored and receives only what the shell printed since — the history is not
// re-sent, and nothing appears twice.
void TestWarmReattachKeepsTheMirrorAndDuplicatesNothing() {
  ShortServerDir dir;
  StartServer(dir);
  const std::filesystem::path root = dir.scratch() / "project";
  std::filesystem::create_directories(root);
  const auto identity = [](const std::filesystem::path& path) { return path; };

  TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 6, 60);
  auto first = Connect(dir);
  auto channel = remote::RemoteTerminalChannel::Open(
      first, terminal::HostTerminalSource::OpenRequest{.working_directory = root, .shell = {"sh"}},
      session, identity);
  Expect(channel != nullptr, "the terminal opens");
  TerminalSessionTestAccess::EnterHostMode(session, channel);
  TypeLine(session, "for i in $(seq 1 20); do echo early-$i; done; sleep 1; echo late-$((6*7))");
  Expect(WaitForText(session, "early-20"), "the early output arrives:\n" + ScreenText(session));
  const std::uint64_t handle = channel->handle();
  first->peer().Fail("link dropped");
  Expect(WaitUntil([&] { return !session.running(); }, std::chrono::seconds(5)), "the link drops");
  const auto resume = session.host_resume_point();
  Expect(resume.has_value(), "a mirrored session has a resume point");

  auto second = Connect(dir);
  auto reattached = remote::RemoteTerminalChannel::Attach(second, handle, session, identity, resume);
  Expect(reattached != nullptr, "the warm reattach is sent");
  session.ReattachHost(reattached);
  Expect(WaitForText(session, "late-42"), "what the shell printed while detached arrives:\n" +
                                              ScreenText(session));
  const std::string text = ScreenText(session);
  for (const char* line : {"early-1\n", "early-20", "late-42"}) {
    Expect(text.find(line) != std::string::npos && text.find(line) == text.rfind(line),
           std::string("exactly one '") + line + "':\n" + text);
  }
  Expect(text.find("connection to the host lost") != std::string::npos,
         "the kept scrollback still says where the link dropped");
  TypeLine(session, "echo again-$((1+1))");
  Expect(WaitForText(session, "again-2"), "and it takes input on the new connection");
  session.Stop();
  channel.reset();
}

// Two clients on one host terminal SHARE it (tmux's rule, spec "Two clients
// share one pty at the smaller size"): the pty takes the smaller pane, both see
// every byte and both can type, a one-shot signal (the bell) reaches each of
// them, and when the smaller one leaves the other gets its room back. A second
// attach used to take the terminal over from the first.
void TestTwoClientsShareAHostTerminalAtTheSmallerSize() {
  ShortServerDir dir;
  StartServer(dir);
  const std::filesystem::path root = dir.scratch() / "project";
  std::filesystem::create_directories(root);
  const auto identity = [](const std::filesystem::path& path) { return path; };

  auto first = Connect(dir);
  TerminalSession a;
  TerminalSessionTestAccess::Reset(a, 20, 80);
  auto channel_a = remote::RemoteTerminalChannel::Open(
      first, terminal::HostTerminalSource::OpenRequest{.working_directory = root, .shell = {"sh"}},
      a, identity);
  Expect(channel_a != nullptr, "the terminal opens");
  TerminalSessionTestAccess::EnterHostMode(a, channel_a);
  a.Resize(20, 80);
  TypeLine(a, "echo ready-$((1+1))");
  Expect(WaitForText(a, "ready-2"), "the first client sees its shell:\n" + ScreenText(a));
  const std::uint64_t handle = channel_a->handle();

  auto second = Connect(dir);
  TerminalSession b;
  TerminalSessionTestAccess::Reset(b, 10, 40);
  auto channel_b = remote::RemoteTerminalChannel::Attach(second, handle, b, identity);
  Expect(channel_b != nullptr, "the second client attaches");
  TerminalSessionTestAccess::EnterHostMode(b, channel_b);
  b.Resize(10, 40);
  Expect(WaitForText(b, "ready-2"), "the second client sees the same screen:\n" + ScreenText(b));

  TypeLine(b, "stty size");
  Expect(WaitForText(b, "10 40") && WaitForText(a, "10 40"),
         "the pty takes the smaller pane, and both see it:\n" + ScreenText(a));
  Expect(WaitUntil([&] { return a.rows() == 10 && a.columns() == 40; }, std::chrono::seconds(5)),
         "the larger client mirrors the shared size");
  TypeLine(a, "echo from-a-$((2+3))");
  Expect(WaitForText(b, "from-a-5"), "what one client types, the other sees:\n" + ScreenText(b));
  (void)a.ConsumeBell();
  (void)b.ConsumeBell();
  TypeLine(b, "printf '\\a'");
  Expect(WaitUntil([&] { return a.ConsumeBell(); }, std::chrono::seconds(5)) &&
             WaitUntil([&] { return b.ConsumeBell(); }, std::chrono::seconds(5)),
         "a bell rings for both clients, not only the first frame's");

  // The second machine closes its tab, then goes away. Its tab's term/close
  // leaves the terminal to the first client: it does not end a shell someone else
  // is using. (Flushed: whether the close was written before the socket closed
  // used to decide whether the first client's shell survived.)
  TerminalSessionTestAccess::EnterHostMode(b, nullptr);
  channel_b.reset();
  Expect(second->peer().Flush(std::chrono::seconds(5)), "the second client's close is sent");
  second.reset();
  // The host's own answer, not a sleep: the remaining client's view is back at
  // its size with no output needed (the detach used to wait for the program to
  // print), and under a sanitizer on a loaded machine the host took longer than
  // the half-second sleep this test once relied on.
  Expect(WaitUntil([&] { return a.rows() == 20 && a.columns() == 80; }, std::chrono::seconds(20)),
         "the host detaches the second client and the remaining one's view is its own size again");
  TypeLine(a, "stty size");
  Expect(WaitForText(a, "20 80"), "the remaining client gets its room back:\n" + ScreenText(a));
  a.Stop();
}

// An attach prefetches only the newest history; scrolling to the top of a host
// terminal asks for the page before it (term/scrollback), until the host holds
// nothing older. Every line arrives exactly once and in order, and the rows the
// mirror already held keep their absolute index (the trim total goes DOWN).
void TestScrollingUpFetchesOlderHostHistory() {
  ShortServerDir dir;
  StartServer(dir);
  const std::filesystem::path root = dir.scratch() / "project";
  std::filesystem::create_directories(root);
  const auto identity = [](const std::filesystem::path& path) { return path; };

  auto first = Connect(dir);
  std::uint64_t handle = 0;
  {
    TerminalSession writer;
    TerminalSessionTestAccess::Reset(writer, 10, 40);
    auto channel = remote::RemoteTerminalChannel::Open(
        first,
        terminal::HostTerminalSource::OpenRequest{
            .working_directory = root, .shell = {"sh"}, .scrollback_lines = 5000,
            .prefetch_lines = 100},
        writer, identity);
    Expect(channel != nullptr, "the terminal opens");
    TerminalSessionTestAccess::EnterHostMode(writer, channel);
    TypeLine(writer, "i=1; while [ $i -le 1500 ]; do echo row-$i; i=$((i+1)); done; echo done-$((2*2))");
    Expect(WaitForText(writer, "done-4"), "the output is written:\n" + ScreenText(writer));
    handle = channel->handle();
    first->peer().Fail("detach");
    TerminalSessionTestAccess::EnterHostMode(writer, nullptr);
  }

  // A second client attaches (the terminal prefetches 100 lines) and pages back.
  auto second = Connect(dir);
  TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 10, 40);
  session.SetMaxScrollbackLines(5000);
  auto channel = remote::RemoteTerminalChannel::Attach(second, handle, session, identity);
  Expect(channel != nullptr, "the reattach is sent");
  TerminalSessionTestAccess::EnterHostMode(session, channel);
  Expect(WaitForText(session, "done-4"), "the attach shows the screen");
  const auto rows = [&]() {
    std::vector<int> numbers;
    for (const auto& line : session.SnapshotLines()) {
      std::string text;
      for (const auto& cell : line.cells) {
        text += cell.DisplayText();
      }
      // "row-N", possibly after a prompt that raced the first line ("$ row-1").
      if (const std::size_t at = text.find("row-"); at != std::string::npos &&
                                                     text.find("echo") == std::string::npos) {
        numbers.push_back(std::stoi(text.substr(at + 4)));
      }
    }
    return numbers;
  };
  // The screen and the prefetched history can land in separate frames: wait for
  // the rows, and never read front() of an empty result (that was a TSAN SEGV).
  Expect(WaitUntil([&] { return !rows().empty(); }, std::chrono::seconds(10)),
         "the attach mirrors the prefetched rows:\n" + ScreenText(session));
  const std::vector<int> prefetched = rows();
  Expect(!prefetched.empty() && prefetched.size() < 200 && prefetched.front() > 1000,
         "only the newest history was prefetched (" + std::to_string(prefetched.size()) + " rows)");
  const std::uint64_t trim_before = session.ScrollbackTrimTotal();
  // Page back until the host has nothing older: the mirror stops growing.
  for (int page = 0; page < 10; ++page) {
    const std::size_t held = session.SnapshotLines().size();
    session.RequestOlderHostHistory();
    if (!WaitUntil([&] { return session.SnapshotLines().size() > held; }, std::chrono::seconds(3))) {
      break;
    }
  }
  const std::vector<int> numbers = rows();
  bool contiguous = numbers.size() == 1500;
  for (std::size_t i = 0; contiguous && i < numbers.size(); ++i) {
    contiguous = numbers[i] == static_cast<int>(i) + 1;
  }
  Expect(contiguous, "every row, once, in order: got " + std::to_string(numbers.size()) +
                         " rows from " + std::to_string(numbers.empty() ? 0 : numbers.front()));
  Expect(session.ScrollbackTrimTotal() < trim_before, "the prepended rows took the trim total down");
  Expect(numbers.empty() || numbers.front() == 1, "paging reached the host's first line");
  session.Stop();
}

// server/status lists every live terminal -- attached, detached, exited -- under
// the workspace root of the connection that opened it, with its directory and
// its attached clients; the per-workspace count is derived from the same rows.
void TestServerStatusListsTerminalsAcrossRoots() {
  ShortServerDir dir;
  StartServer(dir);
  const std::filesystem::path root_a = dir.scratch() / "a";
  const std::filesystem::path root_b = dir.scratch() / "b";
  std::filesystem::create_directories(root_a);
  std::filesystem::create_directories(root_b);
  const auto identity = [](const std::filesystem::path& path) { return path; };
  const auto open = [&](const std::shared_ptr<remote::RemoteServerClient>& client, TerminalSession& session,
                        const std::filesystem::path& cwd) {
    TerminalSessionTestAccess::Reset(session, 24, 80);
    auto channel = remote::RemoteTerminalChannel::Open(
        client, terminal::HostTerminalSource::OpenRequest{.working_directory = cwd, .shell = {"sh"}},
        session, identity);
    Expect(channel != nullptr, "the terminal opens");
    TerminalSessionTestAccess::EnterHostMode(session, channel);
    return channel;
  };

  auto a = ConnectWithRoot(dir, root_a);
  TerminalSession attached;
  auto attached_channel = open(a, attached, root_a);
  TypeLine(attached, "printf '\\033]7;file://localhost%s\\a' \"$PWD\"; echo cwd-$((1+1))");
  Expect(WaitForText(attached, "cwd-2"), "the shell answers:\n" + ScreenText(attached));
  TerminalSession exited;
  auto exited_channel = open(a, exited, root_a);
  TypeLine(exited, "exit");
  Expect(WaitUntil([&] { return !exited.running(); }, std::chrono::seconds(10)), "the second shell exits");
  Expect(exited.host_shell_exited(), "and the session knows its shell ended (not its link)");

  std::uint64_t detached_handle = 0;
  {
    auto b = ConnectWithRoot(dir, root_b);
    TerminalSession left;
    auto left_channel = open(b, left, root_b);
    TypeLine(left, "echo left-$((2+2))");
    Expect(WaitForText(left, "left-4"), "the third shell answers");
    detached_handle = left_channel->handle();
    b->peer().Fail("link dropped");  // nothing is closed on the host
    Expect(WaitUntil([&] { return !left.running(); }, std::chrono::seconds(5)), "its link is gone");
    Expect(!left.host_shell_exited(), "a lost link is not an exited shell");
    TerminalSessionTestAccess::EnterHostMode(left, nullptr);
  }

  std::vector<remote::ServerTerminal> rows;
  Expect(WaitUntil(
             [&] {
               rows = ListedTerminals(dir);
               const remote::ServerTerminal* row = Listed(rows, attached_channel->handle());
               const remote::ServerTerminal* gone = Listed(rows, detached_handle);
               return row != nullptr && row->cwd == root_a.string() && gone != nullptr && gone->clients == 0;
             },
             std::chrono::seconds(10)),
         "the listing catches up with the OSC 7 directory and the dropped link");
  Expect(rows.size() == 3, "three terminals listed: " + std::to_string(rows.size()));
  const remote::ServerTerminal* row = Listed(rows, attached_channel->handle());
  Expect(row != nullptr && row->root == root_a.string() && row->clients == 1 && !row->exited,
         "an attached terminal: its root, its directory, one client");
  row = Listed(rows, exited_channel->handle());
  Expect(row != nullptr && row->root == root_a.string() && row->exited,
         "an exited one is still listed (its output is kept until seen), marked exited");
  row = Listed(rows, detached_handle);
  Expect(row != nullptr && row->root == root_b.string() && row->clients == 0 && !row->exited,
         "a detached one under its own root, attached to nobody");

  std::string error;
  const std::optional<util::JsonValue> raw =
      a->Call(remote::method::kServerStatus, util::JsonValue(util::JsonObject{}), &error);
  Expect(raw.has_value(), "the raw reply: " + error);
  std::int64_t root_a_count = -1;
  for (const util::JsonValue& workspace : (*raw)["workspaces"].AsArray()) {
    if (workspace["root"].AsString() == root_a.string()) {
      root_a_count = workspace["terminals"].AsInt(-1);
    }
  }
  Expect(root_a_count == 2, "the workspace's terminal count comes from the listing: " +
                                std::to_string(root_a_count));
  attached.Stop();
  exited.Stop();
}

// The listing is untrusted on the client: a malformed row is dropped rather than
// guessed at, and a reply with no listing (an older server, whose `terminals` is
// a count) lists nothing -- it is not an error.
void TestServerStatusDecoderTrustsNothing() {
  const auto parse = [](std::string_view text) {
    const std::optional<util::JsonValue> json = util::ParseJson(text);
    Expect(json.has_value(), "test JSON parses");
    return remote::ServerStatusFromJson(*json);
  };
  Expect(!parse(R"({"daemon_epoch":"e1","terminals":3})").terminals.has_value(),
         "an older server's count is not a listing");
  Expect(!parse(R"({"daemon_epoch":"e1"})").terminals.has_value(), "nor is a missing key");
  Expect(parse(R"([1,2])").daemon_epoch.empty(), "a non-object reply reads as nothing reported");
  const remote::ServerStatus status = parse(R"({
    "daemon_epoch": "e1",
    "session_survival": {"kill_user_processes": true},
    "terminals": [
      {"handle": 7, "root": "/srv/p", "cwd": "/srv/p/sub", "clients": 0, "exited": false},
      {"handle": 0, "root": "/srv/p", "clients": 0},
      {"handle": -4, "root": "/srv/p", "clients": 0},
      {"handle": 9007199254740993, "root": "/srv/p", "clients": 0},
      {"handle": "8", "root": "/srv/p", "clients": 0},
      {"handle": 9, "root": 12, "clients": 0},
      {"handle": 10, "root": "/srv/p", "clients": -1},
      {"handle": 11, "root": "/srv/p"},
      {"handle": 12, "root": "", "cwd": 5, "clients": 2, "exited": "yes"},
      "not a row"
    ]})");
  Expect(status.daemon_epoch == "e1", "the epoch");
  Expect(status.session_survival.has_value() && status.session_survival->kill_user_processes,
         "the survival report");
  Expect(status.terminals.has_value() && status.terminals->size() == 2,
         "only the two usable rows survive: " +
             std::to_string(status.terminals.has_value() ? status.terminals->size() : 0));
  if (status.terminals.has_value() && status.terminals->size() == 2) {
    Expect((*status.terminals)[0] == remote::ServerTerminal{.handle = 7, .root = "/srv/p",
                                                            .cwd = "/srv/p/sub", .clients = 0,
                                                            .exited = false},
           "a whole row decodes as sent");
    Expect((*status.terminals)[1] == remote::ServerTerminal{.handle = 12, .root = "", .cwd = "",
                                                            .clients = 2, .exited = false},
           "a mistyped cwd or exited flag reads as not reported");
  }
  std::string many = R"({"terminals":[)";
  for (int i = 1; i <= 1000; ++i) {
    many += (i > 1 ? "," : "") + std::string(R"({"handle":)") + std::to_string(i) +
            R"(,"root":"/r","clients":0})";
  }
  many += "]}";
  Expect(parse(many).terminals->size() == remote::kMaxListedTerminals, "a flood is capped");
}

// A terminal-only session (no workspace root) holds an on-demand server past its
// idle timeout once its connection goes without term/close -- the idle rule
// counts terminals -- and a shell that exits while nobody is attached keeps its
// terminal listed, exited, for the next attach to see.
void TestADetachedTerminalHoldsAnOnDemandServer() {
  ShortServerDir dir;
  std::uint64_t handle = 0;
  {
    auto client = remote::RemoteServerClient::Create();
    std::string error;
    Expect(client->ConnectCommand({MICROIDE_SERVER_BINARY, "attach", "--socket-dir", dir.path().string(),
                                   "--idle-timeout-ms", "100"},
                                  remote::HelloRequest{.release = "test"}, &error),
           "an on-demand server through attach: " + error);
    TerminalSession session;
    TerminalSessionTestAccess::Reset(session, 24, 80);
    auto channel = remote::RemoteTerminalChannel::Open(
        client, terminal::HostTerminalSource::OpenRequest{.shell = {"sh"}}, session,
        [](const std::filesystem::path& path) { return path; });
    Expect(channel != nullptr, "the terminal opens");
    TerminalSessionTestAccess::EnterHostMode(session, channel);
    TypeLine(session, "sleep 1; exit");
    Expect(WaitUntil([&] { return channel->handle() != 0; }, std::chrono::seconds(10)), "it is answered");
    handle = channel->handle();
    channel->Detach();  // what quit does: no term/close
    TerminalSessionTestAccess::EnterHostMode(session, nullptr);
  }  // the client and its attach relay go
  // Twenty idle timeouts with no client: an on-demand server that counted only
  // connections would be gone.
  std::this_thread::sleep_for(std::chrono::milliseconds(2000));
  std::vector<remote::ServerTerminal> rows = ListedTerminals(dir);
  Expect(Listed(rows, handle) != nullptr, "the server outlived its idle timeout and still holds the terminal");
  // The shell's `sleep 1; exit` finishes on the host's own time (a loaded
  // machine starts it late): wait for it rather than assume it.
  Expect(WaitUntil(
             [&] {
               rows = ListedTerminals(dir);
               const remote::ServerTerminal* row = Listed(rows, handle);
               return row != nullptr && row->exited;
             },
             std::chrono::seconds(20), std::chrono::milliseconds(100)),
         "the shell exits while detached, and its terminal is still listed");
  const remote::ServerTerminal* row = Listed(rows, handle);
  Expect(row != nullptr && row->clients == 0 && row->root.empty(),
         "exited while detached, attached to nobody, under no workspace");
}

// A detach that keeps the connection (TD-2026-10-27-399) tells the host: the
// terminal is listed with no client -- what a second machine is offered as
// unclaimed -- while the connection that detached it is still open, and the
// shell keeps running.
void TestADetachThatKeepsTheConnectionReleasesTheTerminal() {
  ShortServerDir dir;
  StartServer(dir);
  auto client = Connect(dir);
  TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);
  auto channel = remote::RemoteTerminalChannel::Open(
      client, terminal::HostTerminalSource::OpenRequest{.shell = {"sh"}}, session,
      [](const std::filesystem::path& path) { return path; });
  Expect(channel != nullptr, "the terminal opens");
  TerminalSessionTestAccess::EnterHostMode(session, channel);
  Expect(WaitUntil([&] { return channel->handle() != 0; }, std::chrono::seconds(10)), "it is answered");
  const std::uint64_t handle = channel->handle();
  {
    const std::vector<remote::ServerTerminal> rows = ListedTerminals(dir);
    const remote::ServerTerminal* row = Listed(rows, handle);
    Expect(row != nullptr && row->clients == 1, "attached: one client");
  }
  channel->Detach();
  TerminalSessionTestAccess::EnterHostMode(session, nullptr);
  Expect(client->connected(), "the connection stays up");
  std::vector<remote::ServerTerminal> rows;
  Expect(WaitUntil(
             [&] {
               rows = ListedTerminals(dir);
               const remote::ServerTerminal* row = Listed(rows, handle);
               return row != nullptr && row->clients == 0;
             },
             std::chrono::seconds(10), std::chrono::milliseconds(20)),
         "the host lists it with no client while the detaching connection is open");
  const remote::ServerTerminal* row = Listed(rows, handle);
  Expect(row != nullptr && !row->exited, "and the shell still runs");
}

// Quit and close-project detach a host terminal rather than close it; a reopen
// reattaches it COLD from the recorded identity -- the screen, what the shell
// printed meanwhile, a dim rule under the restored history. A recorded identity
// from another server process, or a handle the server no longer has, opens a
// fresh shell instead and says which. Closing still ends the terminal.
void TestADetachedHostTerminalIsReattachedByIdentity() {
  ShortServerDir dir;
  StartServer(dir);
  const std::filesystem::path host = dir.scratch() / "host" / "p";
  const std::filesystem::path mirror = dir.scratch() / "mirror" / "p";
  std::filesystem::create_directories(host);
  std::filesystem::create_directories(mirror);
  const remote::RemotePathMap map(mirror, host);

  terminal::HostIdentity recorded;
  {
    auto first = Connect(dir);
    const remote::RemoteProcessLauncher launcher(first, map, {.host_paths_readable_locally = true});
    TerminalSession session;
    Expect(session.Start(launcher, mirror, {}, "sh"), "the host terminal starts");
    TypeLine(session, "echo before-quit-$((6*7)); sleep 1; echo after-quit-$((7*7))");
    Expect(WaitForText(session, "before-quit-42"), "the shell answers:\n" + ScreenText(session));
    recorded = session.host_identity();
    Expect(!recorded.empty() && recorded.epoch == first->hello().daemon_epoch,
           "a running host terminal has an identity: its handle and its server's epoch");
    Expect(session.host_reattach() == terminal::HostReattach::None, "a plain open asked no reattach");
    session.DetachFromHost();
    Expect(!session.running() && session.host_identity() == recorded,
           "detached: not running, the identity kept for the session to record");
  }  // the session (destroyed detached) and the connection go
  {
    const std::vector<remote::ServerTerminal> rows = ListedTerminals(dir);
    const remote::ServerTerminal* row = Listed(rows, recorded.handle);
    Expect(row != nullptr && row->clients == 0 && !row->exited,
           "the host still runs it, attached to nobody: destroying a detached session closes nothing");
  }

  auto second = Connect(dir);
  const remote::RemoteProcessLauncher launcher(second, map, {.host_paths_readable_locally = true});
  TerminalSession reopened;
  Expect(reopened.Start(launcher, mirror, {}, "sh", recorded), "the reattach starts");
  Expect(WaitForText(reopened, "after-quit-49"),
         "the reattached screen has what the shell printed after the quit:\n" + ScreenText(reopened));
  Expect(ScreenText(reopened).find("before-quit-42") != std::string::npos, "and what it printed before");
  Expect(WaitUntil([&] { return reopened.host_reattach() == terminal::HostReattach::Reattached; },
                   std::chrono::seconds(5)),
         "it reports the reattach");
  Expect(reopened.host_identity() == recorded, "the same host terminal");
  Expect(ScreenText(reopened).find("\xe2\x8b\xaf reattached \xe2\x8b\xaf") != std::string::npos,
         "a rule says the history was restored:\n" + ScreenText(reopened));
  TypeLine(reopened, "echo again-$((8*8))");
  Expect(WaitForText(reopened, "again-64"), "and it takes input");

  // Recorded under another server process: a fresh shell, never that handle here.
  TerminalSession restarted;
  Expect(restarted.Start(launcher, mirror, {}, "sh", terminal::HostIdentity{.epoch = "gone", .handle = recorded.handle}),
         "a stale-epoch start");
  Expect(restarted.host_reattach() == terminal::HostReattach::ServerRestarted,
         "reported as a restarted server");
  TypeLine(restarted, "echo fresh-$((3*3))");
  Expect(WaitForText(restarted, "fresh-9"), "a fresh shell:\n" + ScreenText(restarted));
  Expect(restarted.host_identity().handle != recorded.handle, "a different terminal");

  // A handle the server does not have: refused, then a fresh shell.
  TerminalSession gone;
  Expect(gone.Start(launcher, mirror, {}, "sh",
                    terminal::HostIdentity{.epoch = recorded.epoch, .handle = 999'999}),
         "a gone-handle start");
  TypeLine(gone, "echo gone-fresh-$((5*5))");
  Expect(WaitForText(gone, "gone-fresh-25"), "typed input reaches the fresh shell:\n" + ScreenText(gone));
  Expect(gone.host_reattach() == terminal::HostReattach::Gone, "reported as gone");
  Expect(gone.host_identity().handle != 999'999 && !gone.host_identity().empty(),
         "the identity is the new terminal's");

  // Closing ends it on the host.
  reopened.Stop();
  Expect(WaitUntil([&] { return Listed(ListedTerminals(dir), recorded.handle) == nullptr; },
                   std::chrono::seconds(10)),
         "closing a reattached terminal ends it");
  restarted.Stop();
  gone.Stop();
}

// A host terminal asked for before the connection is up is PENDING: it shows
// one status line, drops input (it must not reach a shell that may turn out to be
// another), and starts -- the open it was waiting for -- once the connection is.
void TestAPendingHostTerminalStartsOnceConnected() {
  ShortServerDir dir;
  StartServer(dir);
  const std::filesystem::path host = dir.scratch() / "host" / "p";
  const std::filesystem::path mirror = dir.scratch() / "mirror" / "p";
  std::filesystem::create_directories(host);
  std::filesystem::create_directories(mirror);
  auto connection = std::make_shared<remote::RemoteConnection>();
  const remote::RemoteProcessLauncher launcher(connection, remote::RemotePathMap(mirror, host),
                                               {.host_paths_readable_locally = true});
  Expect(!launcher.Ready(), "no client yet");
  TerminalSession session;
  const terminal::HostIdentity wanted{.epoch = "e", .handle = 4};
  Expect(session.Start(launcher, mirror, {}, "sh", wanted), "the start is accepted");
  Expect(session.host_pending() && !session.running() && !session.is_host_terminal(),
         "pending: no shell, nothing bound");
  Expect(session.host_identity() == wanted, "it names the terminal it will reattach to");
  session.SetPendingStatus("[connecting to fake\xe2\x80\xa6]");
  Expect(ScreenText(session).find("[connecting to fake") != std::string::npos,
         "its one line says what it waits for:\n" + ScreenText(session));
  TypeLine(session, "echo DROPPED");
  (void)session.StartPendingOnHost(launcher);  // not ready yet: it waits again
  Expect(session.host_pending(), "still pending while the host is not connected");

  connection->Replace(Connect(dir));
  Expect(launcher.Ready(), "connected");
  Expect(session.StartPendingOnHost(launcher), "the pending start runs");
  Expect(!session.host_pending(), "no longer pending");
  Expect(session.host_reattach() == terminal::HostReattach::ServerRestarted,
         "the identity it carried was another server's: a fresh shell");
  TypeLine(session, "echo started-$((4*4))");
  Expect(WaitForText(session, "started-16"), "the shell runs on the host:\n" + ScreenText(session));
  Expect(ScreenText(session).find("DROPPED") == std::string::npos, "input typed while pending was dropped");
  session.Stop();
}

}  // namespace

void RegisterRemoteTerminalTests(std::vector<TestCase>& tests) {
  AddTest(tests, "RemoteTerminal/ServerStatusListsTerminalsAcrossRoots",
          TestServerStatusListsTerminalsAcrossRoots);
  AddTest(tests, "RemoteTerminal/ServerStatusDecoderTrustsNothing", TestServerStatusDecoderTrustsNothing);
  AddTest(tests, "RemoteTerminal/ADetachedTerminalHoldsAnOnDemandServer",
          TestADetachedTerminalHoldsAnOnDemandServer);
  AddTest(tests, "RemoteTerminal/ADetachThatKeepsTheConnectionReleasesTheTerminal",
          TestADetachThatKeepsTheConnectionReleasesTheTerminal);
  AddTest(tests, "RemoteTerminal/ADetachedHostTerminalIsReattachedByIdentity",
          TestADetachedHostTerminalIsReattachedByIdentity);
  AddTest(tests, "RemoteTerminal/APendingHostTerminalStartsOnceConnected",
          TestAPendingHostTerminalStartsOnceConnected);
  AddTest(tests, "RemoteTerminal/HostTerminalRunsTheShellOnTheHost",
          TestHostTerminalRunsTheShellOnTheHost);
  AddTest(tests, "RemoteTerminal/HostTerminalSurvivesADroppedConnection",
          TestHostTerminalSurvivesADroppedConnection);
  AddTest(tests, "RemoteTerminal/PredictionLeadsTheEchoByARoundTrip",
          TestPredictionLeadsTheEchoByARoundTrip);
  AddTest(tests, "RemoteTerminal/WarmReattachKeepsTheMirrorAndDuplicatesNothing",
          TestWarmReattachKeepsTheMirrorAndDuplicatesNothing);
  AddTest(tests, "RemoteTerminal/ScrollingUpFetchesOlderHostHistory",
          TestScrollingUpFetchesOlderHostHistory);
  AddTest(tests, "RemoteTerminal/TwoClientsShareAHostTerminalAtTheSmallerSize",
          TestTwoClientsShareAHostTerminalAtTheSmallerSize);
}

}  // namespace microide::tests
