#include "TestSupport.h"
#include "RemoteServerTestSupport.h"
#include "support/FaultRelay.h"
#include "util/CrashPoint.h"

#include "platform/Subprocess.h"
#include "platform/UnixSocket.h"
#include "project/FileWriteGate.h"
#include "project/remote/LinkHeartbeat.h"
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

#include <atomic>
#include <chrono>
#include <mutex>
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
#include <thread>
#include <vector>

extern char** environ;

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
enum class Fault { None, KillClient, KillClientAtDiskStep, PowerCutAtDiskStep, KillServer, DropLink };

std::atomic<std::size_t> g_disk_steps{0};
std::atomic<std::size_t> g_disk_kill_at{std::numeric_limits<std::size_t>::max()};

std::filesystem::path g_power_log_dir;
std::mutex g_power_log_mutex;

void JournalForPowerCut(std::size_t index, const char* site, std::string_view what) {
  const std::lock_guard lock(g_power_log_mutex);
  const std::string_view name(site);
  bool had_bytes = false;
  if (name.ends_with("renaming") || name.ends_with("renaming-deferred") || name.ends_with("renaming-source")) {
    std::error_code error;
    const std::filesystem::path target{std::string(what)};
    if (std::filesystem::is_regular_file(target, error)) {
      had_bytes = std::filesystem::copy_file(target, g_power_log_dir / std::to_string(index),
                                             std::filesystem::copy_options::overwrite_existing, error);
    }
  }
  std::ofstream log(g_power_log_dir / "log", std::ios::app | std::ios::binary);
  log << index << '\t' << site << '\t' << what << '\t' << (had_bytes ? 1 : 0) << '\n';
}

int DiskStepHook(const char* site, std::string_view what) {
  if (std::string_view(site).starts_with("tree-op.")) {
    return 0;
  }
  const std::size_t index = g_disk_steps.fetch_add(1);
  if (!g_power_log_dir.empty()) {
    JournalForPowerCut(index, site, what);
  }
  if (index == g_disk_kill_at.load()) {
    ::raise(SIGKILL);
  }
  return 0;
}

void ApplyPowerCut(const std::filesystem::path& log_dir) {
  struct Undurable {
    std::size_t index = 0;
    bool had_bytes = false;
    bool deferred = false;
  };
  std::map<std::string, Undurable> undurable;
  std::vector<std::string> torn;
  std::ifstream log(log_dir / "log", std::ios::binary);
  std::string line;
  while (std::getline(log, line)) {
    std::vector<std::string> fields;
    for (std::size_t start = 0;;) {
      const std::size_t tab = line.find('\t', start);
      fields.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
      if (tab == std::string::npos) {
        break;
      }
      start = tab + 1;
    }
    if (fields.size() != 4) {
      continue;
    }
    const std::size_t index = util::ParseSize(fields[0]).value_or(0);
    const std::string_view site = fields[1];
    const std::string& what = fields[2];
    if (site.ends_with("renaming") || site.ends_with("renaming-deferred") || site.ends_with("renaming-source")) {
      undurable.try_emplace(what, Undurable{.index = index,
                                            .had_bytes = fields[3] == "1",
                                            .deferred = site.ends_with("deferred")});
    } else if (site.ends_with("directory-synced")) {
      std::erase_if(undurable, [&](const auto& entry) {
        return !entry.second.deferred && std::filesystem::path(entry.first).parent_path() == what;
      });
    } else if (site == "tree.filesystem-synced") {
      undurable.clear();
      torn.clear();
    } else if (site == "mirror.hint-appended" || site == "objects.renamed") {
      torn.push_back(what);
    }
  }
  for (const auto& [target, entry] : undurable) {
    std::error_code error;
    if (entry.had_bytes) {
      std::filesystem::copy_file(log_dir / std::to_string(entry.index), target,
                                 std::filesystem::copy_options::overwrite_existing, error);
    } else {
      std::filesystem::remove(target, error);
    }
  }
  for (const std::string& path : torn) {
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (!error && size > 0) {
      std::filesystem::resize_file(path, size - std::min<std::uintmax_t>(size, 3), error);
    }
  }
}

struct CountDiskSteps {
  explicit CountDiskSteps(std::size_t kill_at = std::numeric_limits<std::size_t>::max()) {
    g_disk_steps = 0;
    g_disk_kill_at = kill_at;
    util::SetCrashPointHook(&DiskStepHook);
  }
  ~CountDiskSteps() { util::SetCrashPointHook(nullptr); }
  CountDiskSteps(const CountDiskSteps&) = delete;
  CountDiskSteps& operator=(const CountDiskSteps&) = delete;
  std::size_t counted() const { return g_disk_steps.load(); }
};

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
    case Fault::KillClientAtDiskStep: return "client killed between disk writes";
    case Fault::PowerCutAtDiskStep: return "power cut between disk writes";
    case Fault::KillServer: return "server killed";
    case Fault::DropLink: return "link dropped";
  }
  return "?";
}

bool IsResolution(Scenario scenario) {
  return scenario == Scenario::KeepLocal || scenario == Scenario::TakeRemote;
}

bool StartsWithALocalChange(Scenario scenario) {
  return scenario == Scenario::PushEdit || scenario == Scenario::PushCreate || scenario == Scenario::PushLarge ||
         scenario == Scenario::PushRename || scenario == Scenario::PushDelete;
}

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

std::string DescribeStore(const remote::MirrorSyncEngine& engine) {
  using State = remote::MirrorSyncEngine::ContentState;
  std::string text = "{";
  for (const std::string_view path : {"a.txt", "b.txt", "big.bin", "n.txt", "r.txt"}) {
    const State state = engine.StateOf(path);
    if (state == State::Unknown) {
      continue;
    }
    text += " " + std::string(path) + "[";
    text += state == State::Current     ? "current"
            : state == State::Stale     ? "stale"
            : state == State::Absent    ? "absent"
            : state == State::Dirty     ? "dirty"
            : state == State::Conflict  ? "conflict"
                                        : "local-only";
    text += "]";
  }
  return text + " }";
}

struct Layout {
  std::filesystem::path socket_dir;
  std::filesystem::path host;
  std::filesystem::path mirror;
};

std::filesystem::path PowerLogDir(const Layout& layout) { return layout.mirror.parent_path() / "power-cut"; }

struct Client {
  std::vector<std::unique_ptr<FaultRelay>> relays;
  std::shared_ptr<remote::RemoteConnection> connection;
  std::unique_ptr<remote::RemoteWorkspace> workspace;
  std::unique_ptr<remote::MirrorStore> store;
  std::unique_ptr<remote::MirrorSyncEngine> engine;

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
        return;
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
  std::size_t disk_steps = 0;
  pid_t killed_server = 0;
};

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
  std::size_t disk_steps = 0;
  {
    const CountDiskSteps count;
    ApplyOperation(scenario, client, layout);
    disk_steps = count.counted();
  }
  const bool fired = client.relay().Disarm();
  return Outcome{.fired = fired,
                 .counted = client.relay().counted(),
                 .disk_steps = disk_steps,
                 .killed_server = fired ? daemon : 0};
}

Outcome RunCrashingClient(Scenario scenario, std::size_t step, const Timing& timing,
                          std::chrono::milliseconds one_way, const Layout& layout, Fault fault,
                          std::chrono::milliseconds stall) {
  const std::vector<std::string> args = {"/proc/self/exe",
                                         std::string(kHelperFlag),
                                         fault == Fault::PowerCutAtDiskStep     ? "power"
                                         : fault == Fault::KillClientAtDiskStep ? "disk"
                                                                                : "frame",
                                         std::to_string(static_cast<int>(scenario)),
                                         std::to_string(step),
                                         std::to_string(timing.has_value() ? timing->count() : -1),
                                         std::to_string(one_way.count()),
                                         layout.socket_dir.string(),
                                         layout.host.string(),
                                         layout.mirror.string(),
                                         std::to_string(stall.count())};
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

void RecoverAndCheck(Scenario scenario, Fault fault, const Outcome& outcome, const Layout& layout,
                     Client& client, const std::string& label) {
  if (outcome.killed_server != 0) {
    WaitUntilTheDaemonIsGone(layout, outcome.killed_server);
    const auto restarted =
        platform::RunSubprocess({MICROIDE_SERVER_BINARY, "start", "--socket-dir", layout.socket_dir.string()},
                                platform::SubprocessOptions{.timeout_ms = 20000});
    Expect(restarted.success(), label + ": the daemon restarts: " + restarted.stderr_text);
  }
  if (fault == Fault::KillClient || fault == Fault::KillClientAtDiskStep || fault == Fault::PowerCutAtDiskStep ||
      client.engine == nullptr) {
    client.Open(layout, std::chrono::milliseconds(0));
  } else {
    client.connection->Replace(nullptr);
    std::string error;
    auto fresh = client.Link(layout, std::chrono::milliseconds(0), &error);
    Expect(fresh != nullptr, label + ": reconnects: " + error);
    client.connection->Replace(fresh);
  }
  const std::string store_at_relaunch = DescribeStore(*client.engine);
  client.Sync();
  const std::string after_first_sync = " (after the first recovery sync: store " + DescribeStore(*client.engine) +
                                       ", host " + Describe(ReadTree(layout.host)) + ", mirror " +
                                       Describe(ReadTree(layout.mirror / "tree")) + "; store at the relaunch " +
                                       store_at_relaunch + ")";
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
  Files expected = Expected(scenario);
  const Files before = {{"a.txt", "base\n"}, {"b.txt", "base\n"}};
  if ((fault == Fault::KillClientAtDiskStep || fault == Fault::PowerCutAtDiskStep) &&
      StartsWithALocalChange(scenario) && mirror == before) {
    expected = before;
  }
  Expect(host == expected, label + ": the host holds exactly " + Describe(expected) + state + after_first_sync);
  Expect(mirror == expected, label + ": the mirror holds exactly " + Describe(expected) + state + after_first_sync);
}

Outcome RunCell(Scenario scenario, Fault fault, std::size_t step, const Timing& timing,
                std::chrono::milliseconds one_way,
                std::chrono::milliseconds helper_stall = std::chrono::milliseconds(0)) {
  const std::string label = std::string(Name(scenario)) + ", " + Name(fault) +
                            (fault == Fault::None ? std::string()
                             : fault == Fault::KillClientAtDiskStep || fault == Fault::PowerCutAtDiskStep
                                 ? " at disk step " + std::to_string(step)
                                 : " at step " + std::to_string(step) + ", " + Name(timing)) +
                            (one_way.count() != 0 ? ", slow link" : "");
  ShortServerDir dir;
  StartServer(dir);
  const Layout layout{.socket_dir = dir.path(), .host = dir.scratch() / "h", .mirror = dir.scratch() / "m"};
  std::filesystem::create_directories(layout.host);
  Prepare(scenario, layout);
  Client client;
  Outcome outcome;
  if (fault == Fault::KillClient || fault == Fault::KillClientAtDiskStep || fault == Fault::PowerCutAtDiskStep) {
    outcome = RunCrashingClient(scenario, step, timing, one_way, layout, fault, helper_stall);
    if (fault == Fault::PowerCutAtDiskStep && outcome.fired) {
      ApplyPowerCut(PowerLogDir(layout));
    }
  } else {
    outcome = RunInProcess(scenario, fault, step, timing, one_way, layout, client);
  }
  RecoverAndCheck(scenario, fault, outcome, layout, client, label);
  return outcome;
}

void RunMatrix(Scenario scenario, Fault fault, std::chrono::milliseconds one_way) {
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
  Expect(fired * 2 >= cells, std::string(Name(scenario)) + ", " + Name(fault) + ": only " +
                                     std::to_string(fired) + " of " + std::to_string(cells) + " faults fired");
}

void RunDiskMatrix(Scenario scenario, Fault fault) {
  const std::size_t steps = RunCell(scenario, Fault::None, 0, std::nullopt, std::chrono::milliseconds(0)).disk_steps;
  Expect(steps > 0, std::string(Name(scenario)) + ": the operation writes to disk");
  if (std::getenv("MICROIDE_FAULT_MATRIX_VERBOSE") != nullptr) {
    std::fprintf(stderr, "[fault matrix] %s, disk: %zu steps\n", Name(scenario), steps);
  }
  std::size_t fired = 0;
  for (std::size_t step = 0; step < steps; ++step) {
    fired += RunCell(scenario, fault, step, std::nullopt, std::chrono::milliseconds(0)).fired
                 ? 1
                 : 0;
  }
  Expect(fired * 2 >= steps, std::string(Name(scenario)) + ", disk: only " + std::to_string(fired) + " of " +
                                 std::to_string(steps) + " crashes fired");
}

constexpr int kSlowOneWayMs = 30;

void TestTheHeartbeatIsNotAStep() {
  const std::chrono::milliseconds past_the_first_ping(
      remote::LinkHeartbeat::Config{}.interval_ms / 4 + 250);
  const Outcome outcome = RunCell(Scenario::PushCreate, Fault::KillClient, 0, std::nullopt,
                                  std::chrono::milliseconds(kSlowOneWayMs), past_the_first_ping);
  Expect(outcome.fired, "the fault still fires, on the operation's first frame");
}

template <Scenario kScenario, Fault kFault, int kOneWayMs>
void TestMatrix() {
  RunMatrix(kScenario, kFault, std::chrono::milliseconds(kOneWayMs));
}

template <Scenario kScenario, Fault kFault>
void TestDiskMatrix() {
  RunDiskMatrix(kScenario, kFault);
}

template <Scenario kScenario>
void AddScenario(std::vector<TestCase>& tests, const std::string& name) {
  const std::string base = "MirrorFaultMatrix/";
  AddTest(tests, base + "Local/" + name + "/KillClient", TestMatrix<kScenario, Fault::KillClient, 0>);
  AddTest(tests, base + "Local/" + name + "/KillServer", TestMatrix<kScenario, Fault::KillServer, 0>);
  AddTest(tests, base + "Local/" + name + "/DropLink", TestMatrix<kScenario, Fault::DropLink, 0>);
  AddTest(tests, base + "Slow/" + name + "/KillClient", TestMatrix<kScenario, Fault::KillClient, kSlowOneWayMs>);
  AddTest(tests, base + "Slow/" + name + "/KillServer", TestMatrix<kScenario, Fault::KillServer, kSlowOneWayMs>);
  AddTest(tests, base + "Slow/" + name + "/DropLink", TestMatrix<kScenario, Fault::DropLink, kSlowOneWayMs>);
  AddTest(tests, base + "Disk/" + name + "/KillClient", TestDiskMatrix<kScenario, Fault::KillClientAtDiskStep>);
  AddTest(tests, base + "Disk/" + name + "/PowerCut", TestDiskMatrix<kScenario, Fault::PowerCutAtDiskStep>);
}

}

int RunMirrorFaultClient(int argc, char** argv) {
  if (argc != 11) {
    std::fprintf(stderr, "%s: expected 9 arguments\n", argv[1]);
    return 2;
  }
  try {
    const std::string_view mode = argv[2];
    const auto scenario = util::ParseInt(argv[3]);
    const auto step = util::ParseSize(argv[4]);
    const auto after_us = util::ParseInt64(argv[5]);
    const auto one_way = util::ParseInt(argv[6]);
    const auto stall_ms = util::ParseInt(argv[10]);
    if ((mode != "frame" && mode != "disk" && mode != "power") || !scenario.has_value() || !step.has_value() ||
        !after_us.has_value() || !one_way.has_value() || !stall_ms.has_value() || *stall_ms < 0) {
      return 2;
    }
    const Layout layout{.socket_dir = argv[7], .host = argv[8], .mirror = argv[9]};
    const Timing timing = *after_us < 0 ? Timing{} : Timing{std::chrono::microseconds(*after_us)};
    Client client;
    client.Open(layout, std::chrono::milliseconds(*one_way));
    if (static_cast<Scenario>(*scenario) == Scenario::WatchPull) {
      client.Subscribe();
    }
    if (mode == "frame") {
      client.relay().Arm(*step, [] { ::raise(SIGKILL); }, false, timing);
      std::this_thread::sleep_for(std::chrono::milliseconds(*stall_ms));
      ApplyOperation(static_cast<Scenario>(*scenario), client, layout);
    } else {
      if (mode == "power") {
        g_power_log_dir = PowerLogDir(layout);
        std::filesystem::create_directories(g_power_log_dir);
      }
      const CountDiskSteps kill_at(*step);
      ApplyOperation(static_cast<Scenario>(*scenario), client, layout);
    }
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "mirror fault client: %s\n", error.what());
    return 3;
  }
}

bool IsMirrorFaultClient(int argc, char** argv) { return argc >= 2 && std::string_view(argv[1]) == kHelperFlag; }

void RegisterMirrorFaultMatrixTests(std::vector<TestCase>& tests) {
  AddTest(tests, "MirrorFaultMatrix/TheHeartbeatIsNotAStep", TestTheHeartbeatIsNotAStep);
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

}
