#include "parity/ParityHarness.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <utility>

#include "TestSupport.h"
#include "parity/LoopbackLocality.h"
#include "parity/ParityKnownGaps.h"
#include "project/remote/MirrorStore.h"
#include "project/remote/MirrorSyncEngine.h"
#include "project/remote/MirrorWriteGate.h"
#include "project/remote/RemoteConnection.h"
#include "project/remote/RemoteProcessLauncher.h"
#include "project/remote/RemoteServerClient.h"
#include "project/remote/RemoteWorkspace.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"

namespace microide::tests::parity {

namespace {

using workspace::WorkspaceShell;
using WorkspaceShellTestAccess = workspace::WorkspaceShell::TestAccess;

void ReplaceAll(std::string& text, std::string_view from, std::string_view to) {
  if (from.empty()) {
    return;
  }
  for (std::size_t at = text.find(from); at != std::string::npos;
       at = text.find(from, at + to.size())) {
    text.replace(at, from.size(), to);
  }
}

std::vector<std::string> Normalize(std::vector<std::string> lines,
                                   const std::filesystem::path& opened_root) {
  // Longest spelling first, so a trailing-separator form does not leave a stray
  // separator behind.
  const std::string root = opened_root.lexically_normal().string();
  for (std::string& line : lines) {
    ReplaceAll(line, root, "<root>");
  }
  std::sort(lines.begin(), lines.end());
  return lines;
}

std::string LineDiff(const std::vector<std::string>& local,
                     const std::vector<std::string>& remote) {
  std::string out;
  std::vector<std::string> only_local;
  std::vector<std::string> only_remote;
  std::set_difference(local.begin(), local.end(), remote.begin(), remote.end(),
                      std::back_inserter(only_local));
  std::set_difference(remote.begin(), remote.end(), local.begin(), local.end(),
                      std::back_inserter(only_remote));
  for (const std::string& line : only_local) {
    out += "\n  - local:    " + line;
  }
  for (const std::string& line : only_remote) {
    out += "\n  + remote:   " + line;
  }
  return out;
}

const KnownGap* FindKnownGap(std::string_view scenario, std::string_view locality) {
  for (const KnownGap& gap : kParityKnownGaps) {
    if (gap.scenario == scenario && gap.locality == locality) {
      return &gap;
    }
  }
  return nullptr;
}

// The mirror locality's stack. The gate flushes the engine after every write, so a
// scenario that reads the host's bytes right after a save sees them: the engine's
// asynchrony is the mirror tests' subject, not parity's.
struct MirrorLocality {
  std::shared_ptr<project::remote::RemoteServerClient> client =
      std::make_shared<project::remote::RemoteServerClient>();
  std::shared_ptr<project::remote::RemoteConnection> connection;
  std::unique_ptr<project::remote::RemoteWorkspace> workspace;
  std::unique_ptr<project::remote::MirrorStore> store;
  std::unique_ptr<project::remote::MirrorSyncEngine> engine;
  std::unique_ptr<project::remote::RemoteProcessLauncher> launcher;

  class FlushingGate final : public project::FileWriteGate {
   public:
    FlushingGate(project::remote::MirrorStore& store, project::remote::MirrorSyncEngine& engine)
        : inner_(store, engine), engine_(engine) {}
    Result WriteText(const std::filesystem::path& path, std::string_view text,
                     Signature signature) override {
      ++writes_;
      Result result = inner_.WriteText(path, text, signature);
      engine_.Flush();
      return result;
    }
    TreeResult ApplyTreeOps(std::span<const TreeOp> ops) override {
      ++writes_;
      TreeResult result = inner_.ApplyTreeOps(ops);
      engine_.Flush();
      return result;
    }
    void DisposeStaged(std::span<const std::filesystem::path> staged) override {
      inner_.DisposeStaged(staged);
    }
    std::size_t writes() const { return writes_; }

   private:
    project::remote::MirrorWriteGate inner_;
    project::remote::MirrorSyncEngine& engine_;
    std::size_t writes_ = 0;
  };
  std::unique_ptr<FlushingGate> gate;

  bool Open(const std::filesystem::path& state_dir, const std::filesystem::path& host_root,
            std::string* error) {
    if (!client->ConnectCommand({MICROIDE_SERVER_BINARY, "serve-stdio"},
                                project::remote::HelloRequest{.release = "parity",
                                                              .root = host_root.string()},
                                error)) {
      return false;
    }
    connection = std::make_shared<project::remote::RemoteConnection>(client);
    workspace = std::make_unique<project::remote::RemoteWorkspace>(connection);
    store = std::make_unique<project::remote::MirrorStore>(state_dir, host_root.filename().string());
    if (!store->Open(error)) {
      return false;
    }
    engine = std::make_unique<project::remote::MirrorSyncEngine>(*workspace, *store);
    engine->RequestSync();
    engine->Flush();
    if (!engine->status().error.empty()) {
      *error = engine->status().error;
      return false;
    }
    gate = std::make_unique<FlushingGate>(*store, *engine);
    launcher = std::make_unique<project::remote::RemoteProcessLauncher>(
        connection, project::remote::RemotePathMap(store->tree(), host_root),
        project::remote::RemoteProcessLauncher::Options{.host_paths_readable_locally = true,
                                                        .description = "mirror"});
    return true;
  }
};

}  // namespace

std::string_view LocalityName(Locality locality) {
  switch (locality) {
    case Locality::kLocal:
      return "local";
    case Locality::kLoopback:
      return "loopback";
    case Locality::kServer:
      return "server";
    case Locality::kMirror:
      return "mirror";
  }
  return "?";
}

void Outcome::Add(std::string_view kind, std::string_view key, std::string_view value) {
  std::string line;
  line.reserve(kind.size() + key.size() + value.size() + 6);
  line.append(kind).append(" | ").append(key).append(" | ").append(value);
  lines_.push_back(std::move(line));
}

RunResult RunUnder(const Scenario& scenario, Locality locality) {
  TemporaryDirectory temp_dir;
  Tree tree;
  tree.locality = locality;
  std::unique_ptr<LoopbackProcessLauncher> loopback_launcher;
  std::unique_ptr<project::remote::RemoteProcessLauncher> server_launcher;
  std::unique_ptr<LoopbackWriteGate> gate;
  std::unique_ptr<MirrorLocality> mirror;
  platform::ProcessLauncher* launcher = nullptr;
  project::FileWriteGate* write_gate = nullptr;
  const RecordingLocalLauncher local_launcher;
  if (locality == Locality::kMirror) {
    tree.host_root = temp_dir.path() / "host" / "project";
    std::filesystem::create_directories(tree.host_root);
    scenario.build(tree.host_root, /*is_mirror=*/false);
    mirror = std::make_unique<MirrorLocality>();
    std::string error;
    Expect(mirror->Open(temp_dir.path() / "mirror", tree.host_root, &error),
           "parity: the mirror locality connects and syncs: " + error);
    tree.root = mirror->store->tree();
    launcher = mirror->launcher.get();
    write_gate = mirror->gate.get();
  } else if (locality == Locality::kLocal) {
    tree.root = temp_dir.path() / "project";
    tree.host_root = tree.root;
    std::filesystem::create_directories(tree.root);
    scenario.build(tree.root, /*is_mirror=*/false);
  } else {
    // Same final component on both sides, so a leaked host path differs from the
    // mirror's only by its parent -- the realistic case, and the one a lazy
    // basename comparison would miss.
    tree.host_root = temp_dir.path() / "host" / "project";
    tree.root = temp_dir.path() / "mirror" / "project";
    std::filesystem::create_directories(tree.host_root);
    std::filesystem::create_directories(tree.root);
    scenario.build(tree.host_root, /*is_mirror=*/false);
    scenario.build(tree.root, /*is_mirror=*/true);
    const LoopbackPathMap map(tree.root, tree.host_root);
    // Writes are replicated by the loopback gate under both: the mirror's sync
    // engine is Phase 2b. What the server locality changes is where every process
    // runs — through the real protocol, on the real daemon.
    gate = std::make_unique<LoopbackWriteGate>(map);
    write_gate = gate.get();
    if (locality == Locality::kLoopback) {
      loopback_launcher = std::make_unique<LoopbackProcessLauncher>(map);
      launcher = loopback_launcher.get();
    } else {
      std::string error;
      server_launcher = ConnectServerLocality(map, &error);
      Expect(server_launcher != nullptr, "parity: the server locality connects: " + error);
      launcher = server_launcher.get();
    }
  }

  Outcome outcome;
  {
    // Declared after the launcher and gate, so it is destroyed before them: the
    // project state points at both until the shell is gone.
    WorkspaceShell shell;
    // The locality is fixed AT the open (project::ProjectLocality): the open is
    // what registers language servers, loads plugins and starts git.
    const bool opened =
        launcher == nullptr
            ? WorkspaceShellTestAccess::OpenProjectTabWithLocality(
                  shell, tree.root,
                  project::ProjectLocality{&local_launcher, &project::LocalFileWriteGate()})
            : WorkspaceShellTestAccess::OpenProjectTabWithLocality(
                  shell, tree.root, project::ProjectLocality{launcher, write_gate});
    Expect(opened, "parity: the project opens under " + std::string(LocalityName(locality)));
    scenario.run(shell, tree, outcome);
  }

  RunResult result;
  result.lines = Normalize(outcome.lines(), tree.root);
  if (loopback_launcher != nullptr) {
    result.spawns = loopback_launcher->spawn_count();
    result.spawn_log = loopback_launcher->spawns();
    result.writes = gate->write_count();
  } else if (server_launcher != nullptr) {
    result.spawns = server_launcher->spawn_count();
    result.spawn_log = server_launcher->recent_spawns();
    result.writes = gate->write_count();
  } else if (mirror != nullptr) {
    result.spawns = mirror->launcher->spawn_count();
    result.spawn_log = mirror->launcher->recent_spawns();
    result.writes = mirror->gate->writes();
  } else {
    result.spawn_log = local_launcher.spawns();
    result.spawns = result.spawn_log.size();
  }
  return result;
}

std::string CheckParity(const Scenario& scenario, Locality locality) {
  const RunResult local = RunUnder(scenario, Locality::kLocal);
  const RunResult loopback = RunUnder(scenario, locality);
  const std::string name = std::string(LocalityName(locality));
  if (local.lines.empty()) {
    return scenario.name + ": the scenario observed nothing, so equality would prove nothing";
  }
  if (scenario.spawns && loopback.spawns == 0) {
    return scenario.name +
           ": the " + name + " launcher saw no spawn, so the project's processes did not go "
           "through it and both runs were local";
  }
  if (scenario.writes && loopback.writes == 0) {
    return scenario.name +
           ": the " + name + " gate saw no write, so the project's writes did not go through it";
  }
  if (loopback.spawns > local.spawns) {
    std::string logs = "\n  local spawns:";
    for (const std::string& spawn : local.spawn_log) {
      logs += "\n    " + spawn;
    }
    logs += "\n  " + name + " spawns:";
    for (const std::string& spawn : loopback.spawn_log) {
      logs += "\n    " + spawn;
    }
    return scenario.name + ": the non-local run started more processes than the local one (" +
           std::to_string(loopback.spawns) + " vs " + std::to_string(local.spawns) +
           ") -- each is a round trip to the host" + logs;
  }
  if (local.lines != loopback.lines) {
    std::string spawn_log;
    for (const std::string& spawn : loopback.spawn_log) {
      spawn_log += "\n    " + spawn;
    }
    return scenario.name + ": a " + name + " (non-local) project differs from the same tree "
                           "opened locally:" +
           LineDiff(local.lines, loopback.lines) + "\n  " + name + " spawns:" +
           (spawn_log.empty() ? std::string(" none") : spawn_log);
  }
  return {};
}

void ExpectParity(const Scenario& scenario) {
  for (const Locality locality : {Locality::kLoopback, Locality::kServer, Locality::kMirror}) {
    const std::string failure = CheckParity(scenario, locality);
    const KnownGap* gap = FindKnownGap(scenario.name, LocalityName(locality));
    if (gap == nullptr) {
      Expect(failure.empty(), failure);
      continue;
    }
    Expect(!failure.empty(), scenario.name + " now matches local under " +
                                 std::string(LocalityName(locality)) +
                                 ": remove it from kParityKnownGaps "
                                 "(tests/parity/ParityKnownGaps.h); it was waiting on " +
                                 std::string(gap->removed_by));
  }
}

}  // namespace microide::tests::parity
