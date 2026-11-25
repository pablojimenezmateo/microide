#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "util/JsonValue.h"

namespace microide::workspace {

struct ProjectWorkspaceState;

struct WorkspaceInvariantViolation {
  std::string rule;
  std::string detail;
};

struct ControlInvariantsQueryOptions {
  bool deep = false;
  std::size_t max_deep_lines = 5'000;
};

struct WorkspaceInvariantReport {
  std::vector<WorkspaceInvariantViolation> violations;
  std::size_t groups = 0;
  std::size_t tabs = 0;
  std::size_t editors = 0;
  bool deep = false;
};

WorkspaceInvariantReport CheckWorkspaceInvariants(const ProjectWorkspaceState& project,
                                                  const ControlInvariantsQueryOptions& options = {});

util::JsonValue BuildControlInvariantsJson(const ProjectWorkspaceState* project,
                                           const ControlInvariantsQueryOptions& options = {});

}
