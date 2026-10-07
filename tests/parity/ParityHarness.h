#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "workspace/shell/WorkspaceShell.h"

namespace microide::tests::parity {

// Local/remote parity: the end-to-end gate of dev-docs/design/remote-projects.md
// § 10.1. One scenario runs under each locality and the two outcomes -- what the
// USER would see, normalized -- must be equal. Equality, not two independent
// assertions: asserting each locality on its own lets both drift together.

enum class Locality { kLocal, kLoopback };
std::string_view LocalityName(Locality locality);

struct Tree {
  Locality locality = Locality::kLocal;
  // The directory the editor opens and does its I/O in (the mirror, remotely).
  std::filesystem::path root;
  // Where the project's processes run and where "the remote's" bytes live. The
  // same directory locally. Read it for host-side bytes; never show it to a user.
  std::filesystem::path host_root;
};

// What a scenario observed, as "kind | key | value" lines. Sorted before
// comparison, and the opened root is rewritten to `<root>` -- the HOST root
// deliberately is not, so a host path reaching the user compares unequal.
class Outcome {
 public:
  void Add(std::string_view kind, std::string_view key, std::string_view value);
  const std::vector<std::string>& lines() const { return lines_; }

 private:
  std::vector<std::string> lines_;
};

struct Scenario {
  std::string name;
  // Materialize the fixture under `root`. Called once for a local run; for a
  // loopback run once for the host (`is_mirror == false`) and once for the mirror,
  // which must not be given what a mirror never holds -- `.git`, above all.
  std::function<void(const std::filesystem::path& root, bool is_mirror)> build;
  std::function<void(workspace::WorkspaceShell& shell, const Tree& tree, Outcome& outcome)> run;
  // Vacuity guards: a loopback run of a scenario that spawns (or writes) must have
  // recorded one, or the loopback was never wired and both runs went local.
  bool spawns = false;
  bool writes = false;
};

struct RunResult {
  std::vector<std::string> lines;  // normalized and sorted
  std::size_t spawns = 0;          // loopback only
  std::size_t writes = 0;          // loopback only
  std::vector<std::string> spawn_log;
};

RunResult RunUnder(const Scenario& scenario, Locality locality);

// Empty when the two runs agree (and the guards hold); otherwise the failure, with
// a line diff. Does not consult the known gaps.
std::string CheckParity(const Scenario& scenario);

// CheckParity plus the known-gaps ratchet (ParityKnownGaps.h): an unlisted
// mismatch fails, and so does a listed gap that now MATCHES, so the list can only
// shrink and nobody has to remember to prune it.
void ExpectParity(const Scenario& scenario);

}  // namespace microide::tests::parity
