#pragma once

// The `invariants` control query: what the workspace's own structure must hold
// at rest, checked from the inside.
//
// Every other oracle a driver has reads a SYMPTOM -- a stale pixel, a label that
// disagrees with its buffer, a file that differs -- and the symptom often shows
// hundreds of actions after the state went wrong: two unsaved buffers under one
// Untitled-N name were reported by the kill -9 restore that lost one of them
// (soak 93ba7c8703e3), never by the split that made them. A replay then has to
// reproduce the whole distance, and a minimizer has nothing short to shrink to.
// These are the rules the code states and relies on, asked directly, so a driver
// that asks after every few actions finds the action that broke one:
//
//   groups       1..kMaxEditorGroups editor groups; the split tree has one leaf
//                per group; the focused group exists
//   tabs         a group's active tab exists; a tab has the payload its kind
//                names; stable ids are unique; the recent-tab list has no id twice
//   untitled     every loaded untitled buffer has a number, and one number names
//                one buffer (two panes of one buffer share it)
//   buffers      a file is one buffer: every loaded editor tab and editable
//                compare side on one path shows the same document (two that do
//                not diverge on the first edit and save over each other)
//   carets       every caret, selection anchor and secondary caret of a loaded
//                editor is inside its buffer
//   caches       with {"deep": true}: the focused editor's derived layout tables
//                agree with a fresh build (TextViewport::VerifyDerivedCachesForTesting),
//                for a buffer of at most `max_deep_lines` lines
//
// Answer: {ok, violations: [{rule, detail}], checked: {groups, tabs, editors,
// deep}}. Read-only: no state change, no redraw.

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
  // The deep check rebuilds the focused buffer's layout: bounded, so a query
  // against a 60'000-line file stays an answer and not a stall.
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

}  // namespace microide::workspace
