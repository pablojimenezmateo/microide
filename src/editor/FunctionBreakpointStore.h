#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace microide::editor {

// One function (symbol) breakpoint. Unlike a line breakpoint it has no file/line —
// the adapter resolves `name` to a location — so these live in a flat list keyed by
// name rather than the path-keyed BreakpointStore. `condition`/`hit_condition` are
// persisted Phase-6-style modifiers; the trailing fields are transient adapter
// results (never persisted, reset on load / session end).
struct FunctionBreakpoint {
  std::string name;
  bool enabled = true;
  std::optional<std::string> condition;
  std::optional<std::string> hit_condition;

  // Transient (not persisted): populated from setFunctionBreakpoints responses.
  bool verified = false;
  int adapter_id = 0;
  std::string verify_message;

  bool operator==(const FunctionBreakpoint& other) const {
    return name == other.name && enabled == other.enabled && condition == other.condition &&
           hit_condition == other.hit_condition;
  }
};

// Adapter verification result for one function breakpoint, mapped from a DAP
// `Breakpoint` (kept DAP-free so this editor-layer store carries no protocol
// dependency). Matched back to a stored breakpoint by the requested name.
struct VerifiedFunctionBreakpoint {
  int id = 0;
  bool verified = false;
  std::string message;
};

// Per-project function-breakpoint state. Adapter-agnostic: the host snapshots it at
// launch, sends setFunctionBreakpoints, and reflects verification back through
// ApplyVerification. Mirrors BreakpointStore's ownership/location (lives on
// ProjectWorkspaceState, survives session restarts).
class FunctionBreakpointStore {
 public:
  // Add a breakpoint on `name`. Returns false (no-op) when `name` is empty or a
  // breakpoint with that name already exists; true when one was added.
  bool Add(std::string name);
  // Remove the breakpoint at `index` (no-op when out of range).
  void Remove(std::size_t index);
  // Flip the enabled state at `index`. Returns true when toggled.
  bool ToggleEnabled(std::size_t index);
  // Edit the modifier fields at `index`. A nullopt clears the field. No-op when out
  // of range or unchanged. Bumps the revision on change.
  void SetCondition(std::size_t index, std::optional<std::string> condition);
  void SetHitCondition(std::size_t index, std::optional<std::string> hit_condition);
  void Clear();

  bool Empty() const { return breakpoints_.empty(); }
  std::size_t Count() const { return breakpoints_.size(); }
  const std::vector<FunctionBreakpoint>& All() const { return breakpoints_; }
  bool HasName(const std::string& name) const;

  // Reflect adapter verification. `requested_names` are the names sent (in order);
  // each result is matched back to a stored breakpoint by its requested name (NOT by
  // array index) so a response landing after the user edited the set still marks the
  // right rows. A name no longer present (removed in flight) is dropped.
  void ApplyVerification(const std::vector<std::string>& requested_names,
                         const std::vector<VerifiedFunctionBreakpoint>& results);
  // Reflect a single asynchronous DAP `breakpoint` event (a function breakpoint the
  // adapter binds/relocates/invalidates after the initial pending response). Matched
  // by the adapter id assigned at setFunctionBreakpoints time. No-op when nothing
  // matches. Returns true when a breakpoint changed.
  bool ApplyBreakpointEvent(const VerifiedFunctionBreakpoint& result);
  // Drop all transient verification state (e.g. when a session terminates).
  void ResetVerification();

  // Replace the entire store (persistence restore). Resets verification; drops
  // empty-named and duplicate entries defensively.
  void ReplaceAll(std::vector<FunctionBreakpoint> breakpoints);

  std::uint64_t revision() const { return revision_; }

 private:
  void BumpRevision();

  std::vector<FunctionBreakpoint> breakpoints_;
  std::uint64_t revision_ = 0;
};

}  // namespace microide::editor
