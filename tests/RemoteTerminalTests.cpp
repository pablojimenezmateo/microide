#include "TestSupport.h"
#include "RemoteServerTestSupport.h"
#include "TerminalSessionTestAccess.h"

#include "platform/UnixSocket.h"
#include "project/remote/RemoteProcessLauncher.h"
#include "project/remote/RemoteServerClient.h"
#include "project/remote/RemoteServerPaths.h"
#include "project/remote/RemoteTerminalChannel.h"
#include "terminal/TerminalSession.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

namespace microide::tests {
namespace {

namespace remote = project::remote;
using terminal::TerminalSession;

#if defined(__unix__) || defined(__APPLE__)

std::shared_ptr<remote::RemoteServerClient> Connect(const ShortServerDir& dir) {
  auto client = std::make_shared<remote::RemoteServerClient>();
  const int fd = platform::ConnectUnixSocket(remote::ServerSocketPath(dir.path()));
  std::string error;
  Expect(fd >= 0 && client->ConnectFds(fd, fd, remote::HelloRequest{.release = "test"}, &error),
         "the client connects: " + error);
  return client;
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

  // The second machine goes away: its socket closes and the host detaches it.
  TerminalSessionTestAccess::EnterHostMode(b, nullptr);
  channel_b.reset();
  second.reset();
  TypeLine(a, "sleep 0.5; stty size");
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
  Expect(rows().size() < 200 && rows().front() > 1000, "only the newest history was prefetched");
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

#endif

}  // namespace

void RegisterRemoteTerminalTests(std::vector<TestCase>& tests) {
#if defined(__unix__) || defined(__APPLE__)
  AddTest(tests, "RemoteTerminal/HostTerminalRunsTheShellOnTheHost",
          TestHostTerminalRunsTheShellOnTheHost);
  AddTest(tests, "RemoteTerminal/HostTerminalSurvivesADroppedConnection",
          TestHostTerminalSurvivesADroppedConnection);
  AddTest(tests, "RemoteTerminal/WarmReattachKeepsTheMirrorAndDuplicatesNothing",
          TestWarmReattachKeepsTheMirrorAndDuplicatesNothing);
  AddTest(tests, "RemoteTerminal/ScrollingUpFetchesOlderHostHistory",
          TestScrollingUpFetchesOlderHostHistory);
  AddTest(tests, "RemoteTerminal/TwoClientsShareAHostTerminalAtTheSmallerSize",
          TestTwoClientsShareAHostTerminalAtTheSmallerSize);
#else
  (void)tests;
#endif
}

}  // namespace microide::tests
