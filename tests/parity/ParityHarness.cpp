#include "parity/ParityHarness.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <utility>

#include "TestSupport.h"
#include "parity/LoopbackLocality.h"
#include "parity/ParityKnownGaps.h"
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
    out += "\n  + loopback: " + line;
  }
  return out;
}

const KnownGap* FindKnownGap(std::string_view scenario) {
  for (const KnownGap& gap : kParityKnownGaps) {
    if (gap.scenario == scenario) {
      return &gap;
    }
  }
  return nullptr;
}

}  // namespace

std::string_view LocalityName(Locality locality) {
  return locality == Locality::kLocal ? "local" : "loopback";
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
  std::unique_ptr<LoopbackProcessLauncher> launcher;
  std::unique_ptr<LoopbackWriteGate> gate;
  if (locality == Locality::kLocal) {
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
    launcher = std::make_unique<LoopbackProcessLauncher>(map);
    gate = std::make_unique<LoopbackWriteGate>(map);
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
            ? WorkspaceShellTestAccess::OpenProjectTab(shell, tree.root, false, false)
            : WorkspaceShellTestAccess::OpenProjectTabWithLocality(
                  shell, tree.root, project::ProjectLocality{launcher.get(), gate.get()});
    Expect(opened, "parity: the project opens under " + std::string(LocalityName(locality)));
    scenario.run(shell, tree, outcome);
  }

  RunResult result;
  result.lines = Normalize(outcome.lines(), tree.root);
  if (launcher != nullptr) {
    result.spawns = launcher->spawn_count();
    result.spawn_log = launcher->spawns();
    result.writes = gate->write_count();
  }
  return result;
}

std::string CheckParity(const Scenario& scenario) {
  const RunResult local = RunUnder(scenario, Locality::kLocal);
  const RunResult loopback = RunUnder(scenario, Locality::kLoopback);
  if (local.lines.empty()) {
    return scenario.name + ": the scenario observed nothing, so equality would prove nothing";
  }
  if (scenario.spawns && loopback.spawns == 0) {
    return scenario.name +
           ": the loopback launcher saw no spawn, so the project's processes did not go "
           "through it and both runs were local";
  }
  if (scenario.writes && loopback.writes == 0) {
    return scenario.name +
           ": the loopback gate saw no write, so the project's writes did not go through it";
  }
  if (local.lines != loopback.lines) {
    std::string spawn_log;
    for (const std::string& spawn : loopback.spawn_log) {
      spawn_log += "\n    " + spawn;
    }
    return scenario.name + ": a loopback (non-local) project differs from the same tree "
                           "opened locally:" +
           LineDiff(local.lines, loopback.lines) + "\n  loopback spawns:" +
           (spawn_log.empty() ? std::string(" none") : spawn_log);
  }
  return {};
}

void ExpectParity(const Scenario& scenario) {
  const std::string failure = CheckParity(scenario);
  const KnownGap* gap = FindKnownGap(scenario.name);
  if (gap == nullptr) {
    Expect(failure.empty(), failure);
    return;
  }
  Expect(!failure.empty(), scenario.name +
                               " now matches local: remove it from kParityKnownGaps "
                               "(tests/parity/ParityKnownGaps.h); it was waiting on " +
                               std::string(gap->removed_by));
}

}  // namespace microide::tests::parity
