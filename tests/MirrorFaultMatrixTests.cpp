#include "TestSupport.h"
#include "RemoteServerTestSupport.h"
#include "support/FaultRelay.h"

#include "platform/Subprocess.h"
#include "platform/UnixSocket.h"
#include "project/FileWriteGate.h"
#include "project/remote/MirrorStore.h"
#include "project/remote/MirrorSyncEngine.h"
#include "project/remote/MirrorWriteGate.h"
#include "project/remote/RemoteConnection.h"
#include "project/remote/RemoteServerClient.h"
#include "project/remote/RemoteServerPaths.h"
#include "project/remote/RemoteWorkspace.h"
#include "util/Parse.h"

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

extern char** environ;

// Remote data safety under fault injection: every step of a push, a pull and a
// conflict resolution, with the client killed, the server killed, or the link
// dropped at that step -- over a direct link and over a slow one -- and then the
// recovery a user gets (the app relaunched on the mirror, or the connection
// re-established), checked for loss on both sides.
//
// A "step" is a wire frame (tests/support/FaultRelay.h counts them once the
// operation starts): the fault lands between every pair of frames the operation
// exchanges, so a request the host never received, one it applied without the
// answer arriving, and a reply the client got but never recorded are each a
// cell. The host side is the real daemon (`microide-server start`), as in
// production, so a dropped link leaves it running and finishing what it had,
// and a killed one is SIGKILLed mid-whatever and restarted for the reconnect.
// The client crash is a real one: the operation runs in this binary re-executed
// as a helper (--mirror-fault-client), which raises SIGKILL on itself at the
// step, so the mirror's disk is exactly what a crash leaves.
//
// Every cell must end -- after reconnecting, syncing, and (for a resolution the
// crash interrupted) choosing the same side again -- with no error, nothing
// unsent, no conflict left, and both trees equal to exactly the expected files:
// the one side's bytes when the operation was one-sided (where a conflict is
// itself a finding), the chosen side's when it was a resolution. No stray file
// on either side (a write's temporary left in the host tree would sync into the
// mirror as a file the user never made).
namespace microide::tests {

int RunMirrorFaultClient(int argc, char** argv);

namespace {

namespace remote = project::remote;
using Resolution = remote::MirrorSyncEngine::Resolution;
using TreeOp = project::FileWriteGate::TreeOp;
using Files = std::map<std::string, std::string>;

constexpr std::string_view kHelperFlag = "--mirror-fault-client";

enum class Scenario {
  PushEdit, PushCreate, PushLarge, PushRename, PushDelete, Replay, Pull, PullLarge, WatchPull, KeepLocal, TakeRemote
};
enum class Fault { None, KillClient, KillServer, DropLink };

// When, relative to the step's frame: before it is delivered (a request the host
// never got, a reply the client never saw), or this long after it was (the peer
// part-way through handling it: a server mid-write, a client mid-record).
using Timing = std::optional<std::chrono::microseconds>;
const Timing kTimings[] = {std::nullopt, std::chrono::microseconds(0), std::chrono::microseconds(300),
                           std::chrono::microseconds(2000)};

std::string Name(const Timing& timing) {
  return timing.has_value() ? std::to_string(timing->count()) + " us after it arrived" : "before it arrived";
}

const char* Name(Scenario scenario) {
  switch (scenario) {
    case Scenario::PushEdit: return "push an edit";
    case Scenario::PushCreate: return "push a new file";
    case Scenario::PushLarge: return "push a large new file";
    case Scenario::PushRename: return "push a rename";
    case Scenario::PushDelete: return "push a delete";
    case Scenario::Replay: return "replay an offline save and rename";
    case Scenario::Pull: return "pull an edit, a new file and a delete";
    case Scenario::PullLarge: return "pull a large new file";
    case Scenario::WatchPull: return "pull what a watch delta announces";
    case Scenario::KeepLocal: return "resolve Keep Local";
    case Scenario::TakeRemote: return "resolve Take Remote";
  }
  return "?";
}

const char* Name(Fault fault) {
  switch (fault) {
    case Fault::None: return "no fault";
    case Fault::KillClient: return "client killed";
    case Fault::KillServer: return "server killed";
    case Fault::DropLink: return "link dropped";
  }
  return "?";
}

bool IsResolution(Scenario scenario) {
  return scenario == Scenario::KeepLocal || scenario == Scenario::TakeRemote;
}

// Over the large-file threshold below, and several bulk chunks long: a push of it
// is a request plus WriteData frames, a pull of it a streamed fetch.
std::string Large(char tag) {
  std::string bytes;
  bytes.reserve(200 * 1024);
  std::uint32_t state = static_cast<std::uint32_t>(tag) * 2654435761u + 1;
  while (bytes.size() < 200 * 1024) {
    state = state * 1664525u + 1013904223u;
    bytes.push_back(static_cast<char>('a' + (state >> 24) % 26));
    if (bytes.size() % 64 == 63) {
      bytes.push_back('\n');
    }
  }
  return bytes;
}

remote::MirrorSyncEngine::Options EngineOptions() {
  remote::MirrorSyncEngine::Options options;
  options.large_file_bytes = 64 * 1024;
  return options;
}

Files Expected(Scenario scenario) {
  switch (scenario) {
    case Scenario::PushEdit: return {{"a.txt", "local\n"}, {"b.txt", "base\n"}};
    case Scenario::PushCreate: return {{"a.txt", "base\n"}, {"b.txt", "base\n"}, {"n.txt", "local\n"}};
    case Scenario::PushLarge: return {{"a.txt", "base\n"}, {"b.txt", "base\n"}, {"big.bin", Large('L')}};
    case Scenario::PushRename: return {{"r.txt", "base\n"}, {"b.txt", "base\n"}};
    case Scenario::PushDelete: return {{"a.txt", "base\n"}};
    case Scenario::Replay: return {{"a.txt", "local\n"}, {"r.txt", "base\n"}};
    case Scenario::Pull:
    case Scenario::WatchPull: return {{"a.txt", "remote\n"}, {"n.txt", "remote\n"}};
    case Scenario::PullLarge: return {{"a.txt", "base\n"}, {"b.txt", "base\n"}, {"big.bin", Large('R')}};
    case Scenario::KeepLocal: return {{"a.txt", "local\n"}, {"b.txt", "base\n"}};
    case Scenario::TakeRemote: return {{"a.txt", "remote\n"}, {"b.txt", "base\n"}};
  }
  return {};
}

Files ReadTree(const std::filesystem::path& root) {
  Files files;
  std::error_code error;
  for (auto it = std::filesystem::recursive_directory_iterator(root, error);
       !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
    if (it->is_directory(error)) {
      continue;
    }
    // Read without throwing: the waits below read the tree while the engine is
    // applying a pull, and a file it deletes between the listing and the read is
    // simply not there (ReadFile throws for it -- an ASan run caught one).
    std::ifstream in(it->path(), std::ios::binary);
    if (in) {
      files[it->path().lexically_relative(root).generic_string()] =
          std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
  }
  return files;
}

std::string Describe(const Files& files) {
  std::string text = "{";
  for (const auto& [path, content] : files) {
    text += " " + path + "=" + (content.size() > 16 ? std::to_string(content.size()) + " bytes" : content.substr(0, content.find('\n')));
  }
  return text + " }";
}

struct Layout {
  std::filesystem::path socket_dir;
  std::filesystem::path host;
  std::filesystem::path mirror;
};

// One client: a link through a FaultRelay to the daemon, and the mirror's store
// and engine. Declared so that the engine goes first and the relay last.
struct Client {
  std::vector<std::unique_ptr<FaultRelay>> relays;
  std::shared_ptr<remote::RemoteConnection> connection;
  std::unique_ptr<remote::RemoteWorkspace> workspace;
  std::unique_ptr<remote::MirrorStore> store;
  std::unique_ptr<remote::MirrorSyncEngine> engine;

  // A new link through a new relay; nullptr when the daemon is not there.
  std::shared_ptr<remote::RemoteServerClient> Link(const Layout& layout, std::chrono::milliseconds one_way,
                                                   std::string* error) {
    const int upstream = platform::ConnectUnixSocket(remote::ServerSocketPath(layout.socket_dir));
    if (upstream < 0) {
      *error = "the daemon's socket refused";
      return nullptr;
    }
    relays.push_back(std::make_unique<FaultRelay>(upstream, one_way));
    auto client = remote::RemoteServerClient::Create();
    const int fd = relays.back()->TakeClientFd();
    if (!client->ConnectFds(fd, fd, remote::HelloRequest{.release = "test", .root = layout.host.string()}, error)) {
      return nullptr;
    }
    return client;
  }

  void Open(const Layout& layout, std::chrono::milliseconds one_way) {
    std::string error;
    auto client = Link(layout, one_way, &error);
    Expect(client != nullptr, "connects: " + error);
    connection = std::make_shared<remote::RemoteConnection>(client);
    workspace = std::make_unique<remote::RemoteWorkspace>(connection);
    store = std::make_unique<remote::MirrorStore>(layout.mirror);
    Expect(store->Open(&error), "the mirror opens: " + error);
    engine = std::make_unique<remote::MirrorSyncEngine>(*workspace, *store, EngineOptions(),
                                                        remote::MirrorSyncEngine::Callbacks{});
  }

  FaultRelay& relay() { return *relays.back(); }

  // What RemoteProject does on every Ready: resubscribe the watch, then sync (the
  // sync's manifest is what a delta's id is compared with). Only where the scenario
  // is about the watch -- elsewhere a delta's frames would be steps too.
  void Subscribe() {
    bool native = false;
    std::string error;
    Expect(workspace->SubscribeWatch(engine->WatchSink(), &native, &error), "the watch subscribes: " + error);
    Sync();
  }

  std::filesystem::path Tree(std::string_view path) const { return store->tree() / std::string(path); }

  void Sync() {
    engine->RequestSync();
    engine->Flush();
  }
};

std::optional<pid_t> DaemonPid(const Layout& layout) {
  const auto status = platform::RunSubprocess(
      {MICROIDE_SERVER_BINARY, "status", "--socket-dir", layout.socket_dir.string(), "--json"},
      platform::SubprocessOptions{.timeout_ms = 10000});
  const std::string& text = status.stdout_text;
  const std::size_t at = text.find("\"pid\":");
  if (at == std::string::npos) {
    return std::nullopt;
  }
  std::size_t begin = text.find_first_of("0123456789", at);
  std::size_t end = text.find_first_not_of("0123456789", begin);
  const auto pid = util::ParseInt64(std::string_view(text).substr(begin, end - begin));
  return pid.has_value() && *pid > 0 ? std::optional<pid_t>(static_cast<pid_t>(*pid)) : std::nullopt;
}

// Until the killed daemon's listening socket refuses: what `start` asks before it
// starts another. A SIGKILLed multi-threaded process can show as a zombie leader
// while its other threads still hold its descriptors, so the process state is not
// the answer -- `start` then found the socket answering, said "already running",
// and nothing served the reconnect.
void WaitUntilTheDaemonIsGone(const Layout& layout, pid_t pid) {
  for (int i = 0; i < 500; ++i) {
    const int fd = platform::ConnectUnixSocket(remote::ServerSocketPath(layout.socket_dir));
    if (fd < 0 && !server_fixture_detail::ProcessIsAlive(pid)) {
      return;
    }
    if (fd >= 0) {
      ::close(fd);
    } else {
      std::FILE* stat = std::fopen(("/proc/" + std::to_string(pid) + "/stat").c_str(), "r");
      if (stat == nullptr) {
        return;
      }
      int read_pid = 0;
      char comm[256] = {};
      char state = 0;
      const int got = std::fscanf(stat, "%d %255s %c", &read_pid, comm, &state);
      std::fclose(stat);
      if (got == 3 && state == 'Z') {
        return;  // refusing, and only a zombie left: its descriptors are closed
      }
    }
    ::usleep(10 * 1000);
  }
}

void ChangeHost(const std::filesystem::path& host) {
  WriteFile(host / "a.txt", "remote\n");
  WriteFile(host / "n.txt", "remote\n");
  std::filesystem::remove(host / "b.txt");
}

void ApplyOperation(Scenario scenario, Client& client, const Layout& layout) {
  remote::MirrorWriteGate gate(*client.store, *client.engine);
  const auto ops = [&](std::vector<TreeOp> list) {
    const auto applied = gate.ApplyTreeOps(list);
    Expect(applied.ok, std::string("the local operation applies: ") + applied.error_message);
    gate.DisposeStaged(applied.staged_directories);
  };
  switch (scenario) {
    case Scenario::PushEdit:
      Expect(gate.WriteText(client.Tree("a.txt"), "local\n").ok, "the save lands");
      break;
    case Scenario::PushCreate:
      Expect(gate.WriteText(client.Tree("n.txt"), "local\n").ok, "the new file lands");
      break;
    case Scenario::PushLarge:
      Expect(gate.WriteText(client.Tree("big.bin"), Large('L')).ok, "the large file lands");
      break;
    case Scenario::PushRename:
      ops({TreeOp{.kind = TreeOp::Kind::Rename, .path = client.Tree("a.txt"), .new_path = client.Tree("r.txt")}});
      break;
    case Scenario::PushDelete:
      ops({TreeOp{.kind = TreeOp::Kind::Delete, .path = client.Tree("b.txt")}});
      break;
    case Scenario::Replay:
    case Scenario::Pull:
    case Scenario::PullLarge:
      client.engine->RequestSync();
      break;
    case Scenario::WatchPull: {
      // The host changes; the delta and its pulls are the steps. Settled when the
      // mirror has it, or the fault has cut the way there.
      ChangeHost(layout.host);
      const Files expected = Expected(Scenario::WatchPull);
      (void)WaitUntil([&] { return ReadTree(layout.mirror / "tree") == expected || client.relay().fired(); },
                      std::chrono::seconds(10), std::chrono::milliseconds(5));
      break;
    }
    case Scenario::KeepLocal:
      client.engine->ResolveConflict("a.txt", Resolution::KeepLocal);
      break;
    case Scenario::TakeRemote:
      client.engine->ResolveConflict("a.txt", Resolution::TakeRemote);
      break;
  }
  client.engine->Flush();
}

// The state before the operation: both sides at the base, synced; then whatever
// the scenario needs (the host's changes for a pull, a parked conflict for a
// resolution). Over a direct link, and the engine closed again, as an app quits.
void Prepare(Scenario scenario, const Layout& layout) {
  WriteFile(layout.host / "a.txt", "base\n");
  WriteFile(layout.host / "b.txt", "base\n");
  Client client;
  client.Open(layout, std::chrono::milliseconds(0));
  client.Sync();
  Expect(client.engine->status().error.empty(), "the first sync: " + client.engine->status().error);
  switch (scenario) {
    case Scenario::Pull:
      ChangeHost(layout.host);
      break;
    case Scenario::Replay: {
      // Saved and renamed while offline: journaled, replayed by the next sync.
      client.engine.reset();
      remote::RemoteWorkspace offline(std::make_shared<remote::RemoteConnection>(nullptr));
      remote::MirrorSyncEngine engine(offline, *client.store, EngineOptions(), remote::MirrorSyncEngine::Callbacks{});
      remote::MirrorWriteGate gate(*client.store, engine);
      Expect(gate.WriteText(client.Tree("a.txt"), "local\n").ok, "the offline save lands");
      const TreeOp rename{.kind = TreeOp::Kind::Rename, .path = client.Tree("b.txt"), .new_path = client.Tree("r.txt")};
      const auto applied = gate.ApplyTreeOps(std::span<const TreeOp>(&rename, 1));
      Expect(applied.ok, "the offline rename applies: " + applied.error_message);
      engine.Flush();
      Expect(engine.status().dirty > 0, "the offline work is journaled");
      break;
    }
    case Scenario::PullLarge:
      WriteFile(layout.host / "big.bin", Large('R'));
      break;
    case Scenario::KeepLocal:
    case Scenario::TakeRemote: {
      WriteFile(layout.host / "a.txt", "remote\n");
      remote::MirrorWriteGate gate(*client.store, *client.engine);
      Expect(gate.WriteText(client.Tree("a.txt"), "local\n").ok, "the local save lands");
      client.engine->Flush();
      Expect(client.engine->Conflicts() == std::vector<std::string>{"a.txt"},
             "the setup parks a conflict on a.txt");
      break;
    }
    default:
      break;
  }
}

struct Outcome {
  bool fired = false;
  std::size_t counted = 0;
  pid_t killed_server = 0;  // KillServer: the daemon the fault killed
};

// The operation over a relay armed at `step`, in this process. The client is
// left as the fault left it, for the recovery.
Outcome RunInProcess(Scenario scenario, Fault fault, std::size_t step, const Timing& timing,
                     std::chrono::milliseconds one_way, const Layout& layout, Client& client) {
  client.Open(layout, one_way);
  if (scenario == Scenario::WatchPull) {
    client.Subscribe();
  }
  std::function<void()> action;
  pid_t daemon = 0;
  if (fault == Fault::KillServer) {
    const std::optional<pid_t> pid = DaemonPid(layout);
    Expect(pid.has_value(), "the daemon's pid is known");
    daemon = pid.value_or(0);
    action = [daemon] { ::kill(daemon, SIGKILL); };
  }
  client.relay().Arm(fault == Fault::None ? std::numeric_limits<std::size_t>::max() : step, action,
                     fault == Fault::DropLink, timing);
  ApplyOperation(scenario, client, layout);
  client.relay().WaitForFault();
  return Outcome{.fired = client.relay().fired(),
                 .counted = client.relay().counted(),
                 .killed_server = client.relay().fired() ? daemon : 0};
}

// The operation in a helper process that SIGKILLs itself at `step`.
Outcome RunCrashingClient(Scenario scenario, std::size_t step, const Timing& timing,
                          std::chrono::milliseconds one_way, const Layout& layout) {
  const std::vector<std::string> args = {"/proc/self/exe",
                                         std::string(kHelperFlag),
                                         std::to_string(static_cast<int>(scenario)),
                                         std::to_string(step),
                                         std::to_string(timing.has_value() ? timing->count() : -1),
                                         std::to_string(one_way.count()),
                                         layout.socket_dir.string(),
                                         layout.host.string(),
                                         layout.mirror.string()};
  std::vector<char*> argv;
  for (const std::string& arg : args) {
    argv.push_back(const_cast<char*>(arg.c_str()));
  }
  argv.push_back(nullptr);
  pid_t child = -1;
  Expect(::posix_spawn(&child, "/proc/self/exe", nullptr, nullptr, argv.data(), environ) == 0,
         "the crashing client starts");
  int status = 0;
  while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
  }
  if (WIFSIGNALED(status)) {
    Expect(WTERMSIG(status) == SIGKILL, "the client died of its own SIGKILL, not " + std::to_string(WTERMSIG(status)));
    return Outcome{.fired = true};
  }
  Expect(WIFEXITED(status) && WEXITSTATUS(status) == 0,
         "the client ran the operation: exit " + std::to_string(WEXITSTATUS(status)));
  return Outcome{.fired = false};
}

// After the fault: what the user's next connection does, then the user settling
// what is left; then the checks.
// The recovery's own link is direct: every fault has landed by then, and its
// correctness does not depend on the link's speed -- only the cost does.
void RecoverAndCheck(Scenario scenario, Fault fault, const Outcome& outcome, const Layout& layout,
                     Client& client, const std::string& label) {
  if (outcome.killed_server != 0) {
    WaitUntilTheDaemonIsGone(layout, outcome.killed_server);
    const auto restarted =
        platform::RunSubprocess({MICROIDE_SERVER_BINARY, "start", "--socket-dir", layout.socket_dir.string()},
                                platform::SubprocessOptions{.timeout_ms = 20000});
    Expect(restarted.success(), label + ": the daemon restarts: " + restarted.stderr_text);
  }
  if (fault == Fault::KillClient || client.engine == nullptr) {
    client.Open(layout, std::chrono::milliseconds(0));  // the relaunch, on the mirror the crash left
  } else {
    // The surviving app: the link's end, then the reconnect (RemoteHostSession:
    // Replace(nullptr) on close, Replace(new) when ready, then RequestSync).
    client.connection->Replace(nullptr);
    std::string error;
    auto fresh = client.Link(layout, std::chrono::milliseconds(0), &error);
    Expect(fresh != nullptr, label + ": reconnects: " + error);
    client.connection->Replace(fresh);
  }
  client.Sync();
  bool conflicted = false;
  std::string conflicts_seen;
  for (int round = 0; round < 3; ++round) {
    const std::vector<std::string> conflicts = client.engine->Conflicts();
    if (conflicts.empty()) {
      break;
    }
    conflicted = true;
    for (const std::string& path : conflicts) {
      conflicts_seen += " " + path;
      // The interrupted resolution, chosen again; a one-sided operation that
      // conflicted is already a finding, settled with the side that moved.
      const Resolution again = scenario == Scenario::TakeRemote || scenario == Scenario::Pull ||
                                       scenario == Scenario::PullLarge || scenario == Scenario::WatchPull
                                   ? Resolution::TakeRemote
                                   : Resolution::KeepLocal;
      client.engine->ResolveConflict(path, again);
    }
    client.engine->Flush();
    client.Sync();
  }
  client.Sync();
  const auto status = client.engine->status();
  const Files host = ReadTree(layout.host);
  const Files mirror = ReadTree(layout.mirror / "tree");
  const std::string state = " (host " + Describe(host) + ", mirror " + Describe(mirror) + ", dirty " +
                            std::to_string(status.dirty) + ", conflicts " + std::to_string(status.conflicts) +
                            ", error '" + status.error + "')";
  Expect(!conflicted || IsResolution(scenario), label + ": a one-sided operation raised a conflict on" +
                                                    conflicts_seen + state);
  Expect(status.error.empty(), label + ": no error after recovering" + state);
  Expect(status.dirty == 0 && status.conflicts == 0, label + ": nothing unsent or in conflict" + state);
  const Files expected = Expected(scenario);
  Expect(host == expected, label + ": the host holds exactly " + Describe(expected) + state);
  Expect(mirror == expected, label + ": the mirror holds exactly " + Describe(expected) + state);
}

// One cell, on a fresh host, mirror and daemon. Returns the frames counted.
Outcome RunCell(Scenario scenario, Fault fault, std::size_t step, const Timing& timing,
                std::chrono::milliseconds one_way) {
  const std::string label = std::string(Name(scenario)) + ", " + Name(fault) +
                            (fault == Fault::None ? std::string()
                                                  : " at step " + std::to_string(step) + ", " + Name(timing)) +
                            (one_way.count() != 0 ? ", slow link" : "");
  ShortServerDir dir;
  StartServer(dir);
  const Layout layout{.socket_dir = dir.path(), .host = dir.scratch() / "h", .mirror = dir.scratch() / "m"};
  std::filesystem::create_directories(layout.host);
  Prepare(scenario, layout);
  Client client;
  Outcome outcome;
  if (fault == Fault::KillClient) {
    outcome = RunCrashingClient(scenario, step, timing, one_way, layout);
  } else {
    outcome = RunInProcess(scenario, fault, step, timing, one_way, layout, client);
  }
  RecoverAndCheck(scenario, fault, outcome, layout, client, label);
  return outcome;
}

void RunMatrix(Scenario scenario, Fault fault, std::chrono::milliseconds one_way) {
  // The steps there are: the operation's frames on an undisturbed run (the
  // cell itself is checked too). A run that happens to exchange fewer frames
  // simply never fires its last step's fault; that cell still recovers.
  const std::size_t steps = RunCell(scenario, Fault::None, 0, std::nullopt, one_way).counted;
  Expect(steps > 0, std::string(Name(scenario)) + ": the operation exchanges frames");
  if (std::getenv("MICROIDE_FAULT_MATRIX_VERBOSE") != nullptr) {
    std::fprintf(stderr, "[fault matrix] %s, %s%s: %zu steps\n", Name(scenario), Name(fault),
                 one_way.count() != 0 ? ", slow link" : "", steps);
  }
  std::size_t cells = 0;
  std::size_t fired = 0;
  for (std::size_t step = 0; step < steps; ++step) {
    for (const Timing& timing : kTimings) {
      ++cells;
      fired += RunCell(scenario, fault, step, timing, one_way).fired ? 1 : 0;
    }
  }
  // Most cells' faults must land, or the matrix is not testing faults. Not all: a
  // run can exchange fewer frames than the undisturbed one did (a watch batches
  // the host's changes into however many deltas its timing makes), and then its
  // last steps have no frame to fire on.
  Expect(fired * 2 >= cells, std::string(Name(scenario)) + ", " + Name(fault) + ": only " +
                                     std::to_string(fired) + " of " + std::to_string(cells) + " faults fired");
}

template <Scenario kScenario, Fault kFault, int kOneWayMs>
void TestMatrix() {
  RunMatrix(kScenario, kFault, std::chrono::milliseconds(kOneWayMs));
}

constexpr int kSlowOneWayMs = 30;

template <Scenario kScenario>
void AddScenario(std::vector<TestCase>& tests, const std::string& name) {
  const std::string base = "MirrorFaultMatrix/";
  AddTest(tests, base + "Local/" + name + "/KillClient", TestMatrix<kScenario, Fault::KillClient, 0>);
  AddTest(tests, base + "Local/" + name + "/KillServer", TestMatrix<kScenario, Fault::KillServer, 0>);
  AddTest(tests, base + "Local/" + name + "/DropLink", TestMatrix<kScenario, Fault::DropLink, 0>);
  AddTest(tests, base + "Slow/" + name + "/KillClient", TestMatrix<kScenario, Fault::KillClient, kSlowOneWayMs>);
  AddTest(tests, base + "Slow/" + name + "/KillServer", TestMatrix<kScenario, Fault::KillServer, kSlowOneWayMs>);
  AddTest(tests, base + "Slow/" + name + "/DropLink", TestMatrix<kScenario, Fault::DropLink, kSlowOneWayMs>);
}

}  // namespace

// The helper: argv = flag scenario step after_us one_way_ms socket_dir host mirror
// (after_us -1: before the frame arrives). Runs
// the operation through a relay that SIGKILLs this process at `step`; exits 0
// when the operation finished first.
int RunMirrorFaultClient(int argc, char** argv) {
  if (argc != 9) {
    std::fprintf(stderr, "%s: expected 7 arguments\n", argv[1]);
    return 2;
  }
  try {
    const auto scenario = util::ParseInt(argv[2]);
    const auto step = util::ParseSize(argv[3]);
    const auto after_us = util::ParseInt64(argv[4]);
    const auto one_way = util::ParseInt(argv[5]);
    if (!scenario.has_value() || !step.has_value() || !after_us.has_value() || !one_way.has_value()) {
      return 2;
    }
    const Layout layout{.socket_dir = argv[6], .host = argv[7], .mirror = argv[8]};
    const Timing timing = *after_us < 0 ? Timing{} : Timing{std::chrono::microseconds(*after_us)};
    Client client;
    client.Open(layout, std::chrono::milliseconds(*one_way));
    if (static_cast<Scenario>(*scenario) == Scenario::WatchPull) {
      client.Subscribe();
    }
    client.relay().Arm(*step, [] { ::raise(SIGKILL); }, false, timing);
    ApplyOperation(static_cast<Scenario>(*scenario), client, layout);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "mirror fault client: %s\n", error.what());
    return 3;
  }
}

bool IsMirrorFaultClient(int argc, char** argv) { return argc >= 2 && std::string_view(argv[1]) == kHelperFlag; }

void RegisterMirrorFaultMatrixTests(std::vector<TestCase>& tests) {
  AddScenario<Scenario::PushEdit>(tests, "PushEdit");
  AddScenario<Scenario::PushCreate>(tests, "PushCreate");
  AddScenario<Scenario::PushLarge>(tests, "PushLarge");
  AddScenario<Scenario::PushRename>(tests, "PushRename");
  AddScenario<Scenario::PushDelete>(tests, "PushDelete");
  AddScenario<Scenario::Replay>(tests, "Replay");
  AddScenario<Scenario::Pull>(tests, "Pull");
  AddScenario<Scenario::PullLarge>(tests, "PullLarge");
  AddScenario<Scenario::WatchPull>(tests, "WatchPull");
  AddScenario<Scenario::KeepLocal>(tests, "ResolveKeepLocal");
  AddScenario<Scenario::TakeRemote>(tests, "ResolveTakeRemote");
}

}  // namespace microide::tests
