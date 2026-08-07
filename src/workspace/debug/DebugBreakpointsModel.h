#pragma once

#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "editor/BreakpointStore.h"
#include "editor/FunctionBreakpointStore.h"
#include "workspace/debug/DapProtocol.h"

namespace microide::workspace {

// One row in the Breakpoints panel (Phase 7). Display strings are prebuilt so the
// (lint-covered) bottom-panel render TU only draws — never materializes strings.
struct DebugBreakpointRowView {
  enum class Kind { Header, ExceptionFilter, Breakpoint, FunctionBreakpoint };
  Kind kind = Kind::Breakpoint;
  std::string display;    // prebuilt: section title / filter label / "file:line" / fn name
  std::string secondary;  // prebuilt muted trailer (e.g. "when x>0"), or empty
  // Exception-filter rows:
  std::string filter_id;
  // True when the active filter advertises supportsCondition (drives the
  // condition-edit affordance). Only meaningful for ExceptionFilter rows.
  bool supports_condition = false;
  bool enabled = false;
  // Breakpoint rows (navigation target):
  std::filesystem::path path;
  std::size_t line = 0;  // 0-based buffer line
  // FunctionBreakpoint rows: the symbol name + the index into the
  // FunctionBreakpointStore (the toggle/remove/condition target).
  std::string function_name;
  std::size_t function_index = 0;
  // True when the active adapter responded but did NOT verify this breakpoint
  // (e.g. no code at the line, or a rejected condition). Drives a warning tint;
  // the reason, when the adapter gave one, is folded into `secondary`. False when
  // verified or when no session has responded yet (a plain not-yet-bound dot).
  bool failed = false;
};

// Backs the "Breakpoints" peer bottom-panel tab (Phase 7): a list of the
// adapter's advertised exception-breakpoint filters (toggle rows reflecting the
// persisted enabled set) followed by the project's line breakpoints (navigable
// rows). The enabled-filter id set is persistent (round-tripped through
// PersistedDebugState); the advertised filters are transient (set when a session
// initializes, cleared on stop). Rows are rematerialized by Rebuild() whenever an
// input changes, mirroring how DebugValueTree keeps a prebuilt flat row list.
class DebugBreakpointsModel {
 public:
  // Persisted: the exception-filter ids the user has enabled. `seeded` records
  // whether we have applied the adapter defaults once, so turning every filter
  // off persists as "all off" rather than re-seeding defaults next session.
  const std::vector<std::string>& EnabledFilterIds() const { return enabled_filter_ids_; }
  bool Seeded() const { return seeded_; }
  void SetEnabledFilterIds(std::vector<std::string> ids, bool seeded);

  // Transient: the adapter's advertised filters for the active session. Seeds the
  // enabled set from each filter's default the first time filters are seen for a
  // project (until then `seeded` is false). Returns true when the enabled set
  // changed (the caller re-sends setExceptionBreakpoints + persists).
  bool SetAdvertisedFilters(const std::vector<dap_protocol::DapExceptionFilter>& filters);
  void ClearAdvertisedFilters();
  const std::vector<dap_protocol::DapExceptionFilter>& AdvertisedFilters() const {
    return advertised_;
  }

  // Toggle one filter's enabled state. No-op if the id is not advertised. Returns
  // true when the set changed.
  bool ToggleFilter(const std::string& filter_id);
  bool IsEnabled(const std::string& filter_id) const;
  // Enabled ids intersected with the advertised filters, in advertised order —
  // the exact `filters` array to send in setExceptionBreakpoints.
  std::vector<std::string> EnabledAdvertisedIds() const;

  // Persisted: per-filter conditions (filterId -> condition expression). Set/cleared
  // independently of the enabled set so a condition survives a disable/enable cycle.
  const std::map<std::string, std::string>& FilterConditions() const { return filter_conditions_; }
  void SetFilterConditions(std::map<std::string, std::string> conditions);
  // Set or clear (nullopt / empty) one filter's condition. Returns true when it
  // changed. No-op when the filter id is not advertised.
  bool SetFilterCondition(const std::string& filter_id, std::optional<std::string> condition);
  // Enabled+advertised filters paired with their condition (empty when none), in
  // advertised order — the input to setExceptionBreakpoints with filterOptions.
  std::vector<std::pair<std::string, std::string>> EnabledFilterOptions() const;

  // Rebuild the prebuilt row list from the current line + function breakpoints +
  // advertised filters. Cheap (breakpoint counts are small); called on any input
  // change.
  void Rebuild(const editor::BreakpointStore& breakpoints,
               const editor::FunctionBreakpointStore& function_breakpoints);

  const std::vector<DebugBreakpointRowView>& Rows() const { return rows_; }
  std::size_t RowCount() const { return rows_.size(); }

 private:
  std::vector<std::string> enabled_filter_ids_;
  std::map<std::string, std::string> filter_conditions_;
  bool seeded_ = false;
  std::vector<dap_protocol::DapExceptionFilter> advertised_;
  std::vector<DebugBreakpointRowView> rows_;
  // Reused across rebuilds so the non-owning file walk allocates nothing in
  // steady state. Views are only live inside Rebuild(); nothing reads this
  // between calls.
  mutable std::vector<editor::BreakpointStore::FileBreakpointsView> file_views_scratch_;
};

}  // namespace microide::workspace
