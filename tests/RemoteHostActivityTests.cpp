#include "TestSupport.h"
#include "RemoteServerTestSupport.h"
#include "TerminalSessionTestAccess.h"

#include "platform/Subprocess.h"
#include "platform/UnixSocket.h"
#include "project/remote/RemoteProtocol.h"
#include "project/remote/RemoteServerClient.h"
#include "project/remote/RemoteServerPaths.h"
#include "project/remote/RemoteTerminalChannel.h"
#include "project/remote/ServerStatusText.h"
#include "server/ProcessActivity.h"
#include "terminal/TerminalSession.h"
#include "util/JsonValue.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

// remote-host-activity: the server's account of what it runs (server/status's
// rows and, with `activity`, their CPU and memory), the subscription that pushes
// it, the client's decoder and `microide-server status`'s table.
namespace microide::tests {
namespace {

namespace remote = project::remote;
using server::ProcessActivity;
using terminal::TerminalSession;

std::shared_ptr<remote::RemoteServerClient> ConnectWithRoot(const ShortServerDir& dir,
                                                            const std::filesystem::path& root) {
  auto client = remote::RemoteServerClient::Create();
  const int fd = platform::ConnectUnixSocket(remote::ServerSocketPath(dir.path()));
  std::string error;
  Expect(fd >= 0 && client->ConnectFds(fd, fd, remote::HelloRequest{.release = "test", .root = root.string()},
                                       &error),
         "the client connects: " + error);
  return client;
}

remote::ServerStatus Parse(std::string_view text) {
  const std::optional<util::JsonValue> json = util::ParseJson(text);
  Expect(json.has_value(), "test JSON parses");
  return remote::ServerStatusFromJson(json.value_or(util::JsonValue(nullptr)));
}

// A stat line is parsed from the LAST ')': a comm may hold spaces and parentheses.
void TestStatLineParsesAroundAHostileComm() {
  // pid (comm) state ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt
  // utime stime cutime cstime prio nice threads itreal starttime vsize rss ...
  const std::string line =
      "4242 (a) b (c) R 17 4242 4242 0 -1 4194304 10 0 0 0 150 50 7 3 20 0 1 0 98765 1000 321 1 2 3\n";
  const std::optional<ProcessActivity::Stat> stat = ProcessActivity::ParseStat(line);
  Expect(stat.has_value(), "the line parses");
  Expect(stat.has_value() && stat->pid == 4242 && stat->ppid == 17, "pid and ppid");
  Expect(stat.has_value() && stat->own_ticks == 200 && stat->ticks == 210,
         "own ticks are utime+stime; the tree's add the reaped children's");
  Expect(stat.has_value() && stat->start_time == 98765 && stat->rss_pages == 321, "start time and rss");
  Expect(!ProcessActivity::ParseStat("").has_value(), "nothing read");
  Expect(!ProcessActivity::ParseStat("4242 (sh R 1 2 3").has_value(), "no closing parenthesis");
  Expect(!ProcessActivity::ParseStat("4242 (sh) R 1 2").has_value(), "truncated before rss");
  Expect(!ProcessActivity::ParseStat("x (sh) R 1 4242 4242 0 -1 0 0 0 0 0 1 1 0 0 20 0 1 0 5 1 2").has_value(),
         "a pid that is not a number");
  Expect(!ProcessActivity::ParseStat("7 (sh) R 1 4242 4242 0 -1 0 0 0 0 0 x 1 0 0 20 0 1 0 5 1 2").has_value(),
         "a tick count that is not a number");
}

// Measure over a synthetic table: subtree sums, the first sample's missing CPU,
// the delta on the second, a reused pid starting over, and a parent loop that
// cannot spin.
void TestMeasureSumsSubtreesAndTakesDeltas() {
  using Stat = ProcessActivity::Stat;
  ProcessActivity activity;
  const auto t0 = std::chrono::steady_clock::time_point{} + std::chrono::seconds(100);
  std::vector<Stat> table = {
      {.pid = 10, .ppid = 1, .ticks = 100, .own_ticks = 100, .start_time = 5, .rss_pages = 10},
      {.pid = 11, .ppid = 10, .ticks = 50, .own_ticks = 50, .start_time = 6, .rss_pages = 20},
      {.pid = 12, .ppid = 11, .ticks = 25, .own_ticks = 25, .start_time = 7, .rss_pages = 30},
      {.pid = 20, .ppid = 1, .ticks = 0, .own_ticks = 0, .start_time = 8, .rss_pages = 1},
  };
  const std::vector<ProcessActivity::Root> roots = {{.pid = 10}, {.pid = 10, .subtree = false}, {.pid = 99}};
  std::vector<ProcessActivity::Figures> first = activity.Measure(table, roots, t0, 100, 4096);
  Expect(first.size() == 3, "one figure per root");
  Expect(first[0].rss_bytes == 60 * 4096 && first[0].cpu_permille == -1,
         "the subtree's memory, and no CPU share on the first sample");
  Expect(first[2].rss_bytes == -1, "a pid that is not there reports nothing");
  table[2].ticks += 100;  // the grandchild spends a second of CPU over two seconds
  std::vector<ProcessActivity::Figures> second =
      activity.Measure(table, roots, t0 + std::chrono::seconds(2), 100, 4096);
  Expect(second[0].cpu_permille == 500, "half a core: " + std::to_string(second[0].cpu_permille));
  // The same pid alone: its own previous sample, not the tree's.
  Expect(second[1].cpu_permille == 0, "the pid alone did not move: " + std::to_string(second[1].cpu_permille));
  table[0].start_time = 999;  // pid 10 is a different process now
  std::vector<ProcessActivity::Figures> reused =
      activity.Measure(table, std::vector<ProcessActivity::Root>{{.pid = 10}}, t0 + std::chrono::seconds(4), 100, 4096);
  Expect(reused[0].cpu_permille == -1, "a reused pid has no previous sample");
  table[2].ticks = 0;  // a child's ticks left with it: never a negative share
  std::vector<ProcessActivity::Figures> dropped =
      activity.Measure(table, std::vector<ProcessActivity::Root>{{.pid = 10}}, t0 + std::chrono::seconds(6), 100, 4096);
  Expect(dropped[0].cpu_permille == 0, "a sum that stepped back reads as idle");
  // A loop in the parent links (impossible in /proc, but this is parsed text).
  std::vector<Stat> loop = {{.pid = 30, .ppid = 31, .rss_pages = 1}, {.pid = 31, .ppid = 30, .rss_pages = 1}};
  Expect(activity.Measure(loop, std::vector<ProcessActivity::Root>{{.pid = 30}}, t0, 100, 4096)[0].rss_bytes == 2 * 4096,
         "each process counted once");
}

// Against the real /proc: a child tree's memory includes its children, and a busy
// loop's share is reported from the second sample on.
void TestSampleSeesARealChildTree() {
  const pid_t tree = ::fork();
  if (tree == 0) {
    ::setpgid(0, 0);  // its own group, so one kill takes the sleeps too
    ::execl("/bin/sh", "sh", "-c", "sleep 30 & sleep 30 & wait", static_cast<char*>(nullptr));
    ::_exit(127);
  }
  const pid_t busy = ::fork();
  if (busy == 0) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    volatile unsigned long spin = 0;
    while (std::chrono::steady_clock::now() < end) {
      spin = spin + 1;
    }
    ::_exit(0);
  }
  ProcessActivity activity;
  const std::vector<ProcessActivity::Root> roots = {{.pid = tree}, {.pid = tree, .subtree = false}, {.pid = busy}};
  // The two sleeps must exist before the tree is measured.
  std::vector<ProcessActivity::Figures> first;
  Expect(WaitUntil(
             [&] {
               first = activity.Sample(roots);
               return first[0].rss_bytes > first[1].rss_bytes && first[1].rss_bytes > 0;
             },
             std::chrono::seconds(10), std::chrono::milliseconds(50)),
         "the shell's tree holds more than the shell");
  Expect(first[2].cpu_permille == -1 || first[2].cpu_permille >= 0, "a figure or not reported");
  // The sample reads only these trees (down the children files); a walk of every
  // process on the box finds the same tree.
  {
    std::vector<ProcessActivity::Stat> everything;
    for (const auto& entry : std::filesystem::directory_iterator("/proc")) {
      const std::string name = entry.path().filename().string();
      if (name.empty() || name[0] < '1' || name[0] > '9') {
        continue;
      }
      // A /proc file reports size 0: read it as a stream.
      std::ifstream file(entry.path() / "stat");
      std::string line;
      if (std::getline(file, line)) {
        if (const auto stat = ProcessActivity::ParseStat(line); stat.has_value()) {
          everything.push_back(*stat);
        }
      }
    }
    ProcessActivity walked;
    const std::vector<ProcessActivity::Figures> whole =
        walked.Measure(everything, roots, std::chrono::steady_clock::now(), 100, ::sysconf(_SC_PAGESIZE));
    const std::vector<ProcessActivity::Figures> trees = ProcessActivity().Sample(roots);
    Expect(whole[0].rss_bytes > whole[1].rss_bytes && trees[0].rss_bytes > trees[1].rss_bytes,
           "both see the shell's children: " + std::to_string(whole[0].rss_bytes) + "/" +
               std::to_string(whole[1].rss_bytes) + " " + std::to_string(trees[0].rss_bytes) + "/" +
               std::to_string(trees[1].rss_bytes));
    // Resident sizes move a page or two between two reads; the tree's members do not.
    const auto close = [](std::int64_t a, std::int64_t b) { return std::abs(a - b) <= 64 * 4096; };
    Expect(close(whole[0].rss_bytes, trees[0].rss_bytes),
           "the tree read down the children files is the tree a full walk finds: " +
               std::to_string(whole[0].rss_bytes) + " vs " + std::to_string(trees[0].rss_bytes));
  }
  ProcessActivity fresh;
  Expect(fresh.Sample(roots)[2].cpu_permille == -1, "no share on a pid's first sample");
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  const std::vector<ProcessActivity::Figures> second = fresh.Sample(roots);
  Expect(second[2].cpu_permille > 100, "a busy loop uses a good part of a core: " +
                                           std::to_string(second[2].cpu_permille));
  ::kill(-tree, SIGKILL);
  ::kill(busy, SIGKILL);
  ::waitpid(tree, nullptr, 0);
  ::waitpid(busy, nullptr, 0);
}

// server/status lists a terminal and a kept process under their own roots, with
// pid, argv and the detach age; the figures appear only when asked.
void TestStatusListsTerminalsAndProcessesAcrossRoots() {
  ShortServerDir dir;
  StartServer(dir);
  const std::filesystem::path root_a = dir.scratch() / "a";
  const std::filesystem::path root_b = dir.scratch() / "b";
  std::filesystem::create_directories(root_a);
  std::filesystem::create_directories(root_b);

  auto a = ConnectWithRoot(dir, root_a);
  TerminalSession shell;
  TerminalSessionTestAccess::Reset(shell, 24, 80);
  auto channel = remote::RemoteTerminalChannel::Open(
      a, terminal::HostTerminalSource::OpenRequest{.working_directory = root_a, .shell = {"sh"}}, shell,
      [](const std::filesystem::path& path) { return path; });
  Expect(channel != nullptr, "the terminal opens");
  TerminalSessionTestAccess::EnterHostMode(shell, channel);

  int kept_pid = -1;
  std::uint64_t kept_handle = 0;
  {
    auto b = ConnectWithRoot(dir, root_b);
    const remote::RemoteServerClient::Spawned spawned =
        b->Spawn({"sleep", "120"}, root_b, {}, /*keep_on_detach=*/true, {});
    if (spawned.handle == 0) {
      Expect(false, "spawned: " + spawned.error);
      return;
    }
    kept_handle = spawned.handle;
    kept_pid = spawned.pid;
    b->peer().Fail("link dropped");
  }

  auto observer = ConnectWithRoot(dir, root_a);
  std::string error;
  std::optional<remote::ServerStatus> status;
  Expect(WaitUntil(
             [&] {
               status = observer->Status(&error);
               if (!status.has_value() || !status->processes.has_value()) {
                 return false;
               }
               for (const remote::ServerProcess& process : *status->processes) {
                 if (process.handle == kept_handle && process.detached_for.has_value()) {
                   return true;
                 }
               }
               return false;
             },
             std::chrono::seconds(10)),
         "the kept process is listed as detached: " + error);
  const remote::ServerProcess* kept = nullptr;
  for (const remote::ServerProcess& process : *status->processes) {
    if (process.handle == kept_handle) {
      kept = &process;
    }
  }
  Expect(kept != nullptr && kept->root == root_b.string() && kept->kept && kept->pid == kept_pid &&
             kept->cwd == root_b.string() && !kept->exited,
         "under its spawning root, kept, with its pid and directory");
  Expect(kept != nullptr && kept->argv.size() == 2 && kept->argv[0] == "sleep",
         "with the argv it was spawned with");
  Expect(kept != nullptr && !kept->figures.rss_bytes.has_value(), "no figures without `activity`");
  Expect(status->terminals.has_value() && status->terminals->size() == 1 &&
             status->terminals->front().root == root_a.string() && status->terminals->front().pid.has_value() &&
             status->terminals->front().scrollback_lines.has_value(),
         "the terminal, with its shell's pid and its line count");
  bool counted = false;
  for (const remote::ServerWorkspace& workspace : status->workspaces) {
    if (workspace.root == root_a.string()) {
      counted = workspace.terminals == 1 && workspace.processes == 0 && workspace.clients == 2;
    }
  }
  Expect(counted, "the workspace's counts come from the rows");
  Expect(status->uptime.has_value() && status->pid.has_value() && !status->server.rss_bytes.has_value(),
         "the server's uptime, and no figures");

  // With activity: figures on every live row, the CPU share once a previous
  // sample exists.
  (void)observer->Status(&error, /*activity=*/true);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  status = observer->Status(&error, /*activity=*/true);
  Expect(status.has_value() && status->server.rss_bytes.value_or(0) > 0 && status->server.cpu_permille.has_value(),
         "the server's own memory and share");
  for (const remote::ServerProcess& process : status->processes.value_or(std::vector<remote::ServerProcess>{})) {
    if (process.handle == kept_handle) {
      Expect(process.figures.rss_bytes.value_or(0) > 0 && process.figures.cpu_permille.has_value(),
             "the kept process's memory and share");
    }
  }
  Expect(status->terminals.has_value() && !status->terminals->empty() &&
             status->terminals->front().figures.rss_bytes.value_or(0) > 0,
         "the terminal's shell tree's memory");

  Expect(status->terminals.has_value() && !status->terminals->empty() &&
             status->terminals->front().command.empty(),
         "an idle shell has nothing in its foreground but itself: no command");

  // What runs in the terminal names its row (TD-2026-10-27-407): the foreground
  // process group's command, not the directory.
  for (const char c : std::string_view("sleep 30")) {
    TerminalSession::KeyPress press;
    press.key = TerminalSession::KeyPress::Key::Char;
    press.codepoint = static_cast<unsigned char>(c);
    (void)shell.SendKeyPress(press);
  }
  TerminalSession::KeyPress enter;
  enter.key = TerminalSession::KeyPress::Key::Enter;
  (void)shell.SendKeyPress(enter);
  Expect(WaitUntil(
             [&] {
               status = observer->Status(&error, /*activity=*/true);
               return status.has_value() && status->terminals.has_value() && !status->terminals->empty() &&
                      status->terminals->front().command == "sleep";
             },
             std::chrono::seconds(10)),
         "a terminal running `sleep 30` says so: '" +
             (status.has_value() && status->terminals.has_value() && !status->terminals->empty()
                  ? status->terminals->front().command
                  : std::string("?")) +
             "'");

  // The table names both.
  const std::string table = remote::FormatServerStatusTable(*status);
  Expect(table.find(root_b.string()) != std::string::npos && table.find("sleep 120") != std::string::npos &&
             table.find("kept ") != std::string::npos && table.find("terminal #") != std::string::npos &&
             table.find("(sleep)") != std::string::npos,
         "the table lists the terminal and the kept process:\n" + table);

  // proc/end from a client that did not spawn it: the kept process ends, and --
  // with nobody holding it to release it -- leaves the listing.
  util::JsonObject end;
  end["handle"] = util::JsonValue(static_cast<std::int64_t>(kept_handle));
  observer->peer().Notify(remote::method::kProcEnd, util::JsonValue(std::move(end)));
  Expect(WaitUntil([&] { return ::kill(kept_pid, 0) != 0; }, std::chrono::seconds(10)), "the process ends");
  Expect(WaitUntil(
             [&] {
               status = observer->Status(&error);
               return status.has_value() && status->processes.has_value() && status->processes->empty();
             },
             std::chrono::seconds(10)),
         "and is forgotten");
  shell.Stop();
}

// activity/subscribe pushes the status every interval while subscribed, and
// nothing once the last subscriber unsubscribed; a subscriber whose connection
// closes is dropped.
void TestActivitySubscriptionPushesOnlyWhileSubscribed() {
  ShortServerDir dir;
  StartServer(dir);
  const std::filesystem::path root = dir.scratch() / "r";
  std::filesystem::create_directories(root);
  auto client = ConnectWithRoot(dir, root);
  std::atomic<int> pushes{0};
  std::string error;
  const std::optional<remote::ServerStatus> now =
      client->SubscribeActivity([&](const remote::ServerStatus&) { ++pushes; }, &error);
  Expect(now.has_value() && now->server.rss_bytes.has_value() && now->activity_subscribers == 1,
         "the reply is the status with activity: " + error);
  Expect(WaitUntil([&] { return pushes.load() >= 2; }, std::chrono::seconds(15)), "pushes arrive every interval");
  client->UnsubscribeActivity();
  // A push the server sent before it read the unsubscribe may still be in flight.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  const int settled = pushes.load();
  std::this_thread::sleep_for(remote::kActivityInterval * 2 + std::chrono::milliseconds(500));
  Expect(pushes.load() == settled, "no push after the last unsubscribe");
  std::optional<remote::ServerStatus> status = client->Status(&error);
  Expect(status.has_value() && status->activity_subscribers == 0, "and the server holds no subscriber");

  {
    auto gone = ConnectWithRoot(dir, root);
    Expect(gone->SubscribeActivity([](const remote::ServerStatus&) {}, &error).has_value(), "subscribed");
    gone->peer().Fail("link dropped");
  }
  Expect(WaitUntil(
             [&] {
               status = client->Status(&error);
               return status.has_value() && status->activity_subscribers == 0;
             },
             std::chrono::seconds(10)),
         "a subscriber whose connection closed is dropped");
}

// Every field of the reply is untrusted: a hostile row is dropped or its field
// reads as not reported, never as a wrong number.
void TestDecoderTrustsNoActivityField() {
  const remote::ServerStatus status = Parse(R"({
    "release": "2.1", "pid": 77,
    "server": {"uptime_ms": 5000, "rss_bytes": "lots", "cpu_permille": -1, "activity_subscribers": 2},
    "workspaces": [
      {"root": "/srv/p", "clients": 1, "terminals": 1, "processes": 99999999999999999, "files": 12,
       "hashed": -3, "watch": "telepathy"},
      {"root": 5}
    ],
    "terminals": [
      {"handle": 3, "root": "/srv/p", "clients": 0, "pid": -4, "scrollback_lines": 100,
       "rss_bytes": 4096, "cpu_permille": 250}
    ],
    "processes": [
      {"handle": 9, "root": "/srv/p", "argv": ["clangd", "--log=error"], "pid": 1234, "kept": true,
       "detached_ms": 720000, "buffered_bytes": 10, "rss_bytes": 1048576, "cpu_permille": 3},
      {"handle": 10, "root": "/srv/p", "argv": ["x", 7]},
      {"handle": 11, "root": "/srv/p", "argv": "x"},
      {"handle": -1, "root": "/srv/p", "argv": []},
      {"handle": 12, "root": "/srv/p", "argv": [], "pid": 99999999999, "rss_bytes": -5, "cpu_permille": 1e9}
    ]
  })");
  Expect(status.release == "2.1" && status.pid == 77, "release and pid");
  Expect(status.uptime == std::chrono::milliseconds(5000) && !status.server.rss_bytes.has_value() &&
             !status.server.cpu_permille.has_value() && status.activity_subscribers == 2,
         "a mistyped figure and the first sample's -1 read as not reported");
  Expect(status.workspaces.size() == 1, "a workspace with no root is dropped");
  Expect(status.workspaces.front().files == std::optional<std::size_t>(12) &&
             !status.workspaces.front().hashed.has_value() && status.workspaces.front().processes == 0 &&
             status.workspaces.front().watch.empty(),
         "out-of-range counts and an unknown watch mode read as not reported");
  Expect(status.terminals.has_value() && status.terminals->size() == 1 && !status.terminals->front().pid.has_value() &&
             status.terminals->front().figures.cpu_permille == 250,
         "a negative pid is not a pid; the figures are kept");
  Expect(status.processes.has_value() && status.processes->size() == 2, "rows with bad argv or handle are dropped");
  const remote::ServerProcess& clangd = status.processes->front();
  Expect(clangd.kept && clangd.detached_for == std::chrono::milliseconds(720000) &&
             clangd.figures.rss_bytes == 1048576 && clangd.argv.size() == 2,
         "a kept process with its age and memory");
  const remote::ServerProcess& odd = status.processes->back();
  Expect(!odd.pid.has_value() && !odd.figures.rss_bytes.has_value() && !odd.figures.cpu_permille.has_value(),
         "an absurd pid, a negative size and a non-integer share read as not reported");
  Expect(!Parse(R"({"processes": 3})").processes.has_value(), "an older server's count is not a listing");
}

void TestStatusTableAndFigureWords() {
  Expect(remote::FormatCpuShare(std::nullopt) == remote::kNotReported, "not reported");
  Expect(remote::FormatCpuShare(32) == "3.2%" && remote::FormatCpuShare(1500) == "150.0%", "per mille of a core");
  Expect(remote::FormatBytes(41 * 1024 * 1024) == "41 MB" && remote::FormatBytes(820 * 1024) == "820 KB" &&
             remote::FormatBytes(std::int64_t{3} << 30) == "3.0 GB",
         "bytes");
  Expect(remote::FormatAge(std::chrono::seconds(12)) == "12 s" && remote::FormatAge(std::chrono::minutes(12)) == "12 min" &&
             remote::FormatAge(std::chrono::minutes(185)) == "3 h 5 min" &&
             remote::FormatAge(std::chrono::hours(52)) == "2 d 4 h",
         "ages");
  Expect(remote::ProcessDisplayName({"/usr/bin/clangd", "--background-index"}) == "clangd --background-index",
         "the program's directory is dropped");
  Expect(remote::ProcessDisplayName({"x", std::string(100, 'a')}, 10).size() < 16, "long names are cut");
  Expect(remote::TerminalPlace("/srv/p", "/srv/p") == "." && remote::TerminalPlace("/srv/p/src", "/srv/p") == "src" &&
             remote::TerminalPlace("/srv/pp", "/srv/p") == "/srv/pp" && remote::TerminalPlace("", "/srv/p").empty(),
         "a terminal's place against its root");
  const remote::ServerStatus status = Parse(R"({
    "release": "2.1", "pid": 77, "server": {"uptime_ms": 7200000, "rss_bytes": 8388608, "cpu_permille": 4},
    "workspaces": [{"root": "/srv/p", "clients": 1, "terminals": 1, "processes": 1, "files": 1520, "watch": "native"}],
    "terminals": [{"handle": 3, "root": "/srv/p", "cwd": "/srv/p/sub", "clients": 2, "pid": 4242,
                   "rss_bytes": 42991616, "cpu_permille": 32}],
    "processes": [{"handle": 9, "root": "/srv/p", "argv": ["clangd"], "pid": 4300, "kept": true, "detached_ms": 720000,
                   "rss_bytes": 188743680, "cpu_permille": 0},
                  {"handle": 4, "root": "/srv/gone", "argv": ["make", "-j8"], "pid": 5000}]
  })");
  const std::string table = remote::FormatServerStatusTable(status);
  const std::string expected =
      "microide-server 2.1 \xc2\xb7 pid 77 \xc2\xb7 up 2 h \xc2\xb7 0.4% \xc2\xb7 8 MB\n"
      "\n"
      "/srv/p \xc2\xb7 1 client \xc2\xb7 1520 files \xc2\xb7 watch native\n"
      "    terminal #3 sub                         pid 4242    2 clients           3.2% \xc2\xb7 41 MB\n"
      "    clangd                                  pid 4300    kept 12 min         0.0% \xc2\xb7 180 MB\n"
      "\n"
      "/srv/gone \xc2\xb7 not open\n"
      "    make -j8                                pid 5000    running             \xe2\x80\xa6 \xc2\xb7 \xe2\x80\xa6\n";
  Expect(table == expected, "the table:\n" + table + "\nexpected:\n" + expected);
}

}  // namespace

void RegisterRemoteHostActivityTests(std::vector<TestCase>& tests) {
  AddTest(tests, "RemoteHostActivity/StatLineParsesAroundAHostileComm", TestStatLineParsesAroundAHostileComm);
  AddTest(tests, "RemoteHostActivity/MeasureSumsSubtreesAndTakesDeltas", TestMeasureSumsSubtreesAndTakesDeltas);
  AddTest(tests, "RemoteHostActivity/SampleSeesARealChildTree", TestSampleSeesARealChildTree);
  AddTest(tests, "RemoteHostActivity/StatusListsTerminalsAndProcessesAcrossRoots",
          TestStatusListsTerminalsAndProcessesAcrossRoots);
  AddTest(tests, "RemoteHostActivity/SubscriptionPushesOnlyWhileSubscribed",
          TestActivitySubscriptionPushesOnlyWhileSubscribed);
  AddTest(tests, "RemoteHostActivity/DecoderTrustsNoActivityField", TestDecoderTrustsNoActivityField);
  AddTest(tests, "RemoteHostActivity/StatusTableAndFigureWords", TestStatusTableAndFigureWords);
}

}  // namespace microide::tests
