#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "workspace/shell/WorkspaceShell.h"

namespace microide::tests::parity {

enum class Locality { kLocal, kLoopback, kServer, kMirror };
std::string_view LocalityName(Locality locality);

struct Tree {
  Locality locality = Locality::kLocal;
  std::filesystem::path root;
  std::filesystem::path host_root;
};

class Outcome {
 public:
  void Add(std::string_view kind, std::string_view key, std::string_view value);
  const std::vector<std::string>& lines() const { return lines_; }

 private:
  std::vector<std::string> lines_;
};

struct Scenario {
  std::string name;
  std::function<void(const std::filesystem::path& root, bool is_mirror)> build;
  std::function<void(workspace::WorkspaceShell& shell, const Tree& tree, Outcome& outcome)> run;
  bool spawns = false;
  bool writes = false;
};

struct RunResult {
  std::vector<std::string> lines;
  std::size_t spawns = 0;
  std::size_t writes = 0;
  std::vector<std::string> spawn_log;
};

RunResult RunUnder(const Scenario& scenario, Locality locality);

std::string CheckParity(const Scenario& scenario, Locality locality);

void ExpectParity(const Scenario& scenario);

}
