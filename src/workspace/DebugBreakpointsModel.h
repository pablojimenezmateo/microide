#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "editor/BreakpointStore.h"
#include "workspace/DapProtocol.h"

namespace microide::workspace {

// One row in the Breakpoints panel (Phase 7). Display strings are prebuilt so the
// (lint-covered) bottom-panel render TU only draws — never materializes strings.
struct DebugBreakpointRowView {
  enum class Kind { Header, ExceptionFilter, Breakpoint };
  Kind kind = Kind::Breakpoint;
  std::string display;    // prebuilt: section title / filter label / "file:line"
  std::string secondary;  // prebuilt muted trailer (e.g. "when x>0"), or empty
  // Exception-filter rows:
  std::string filter_id;
  bool enabled = false;
  // Breakpoint rows (navigation target):
  std::filesystem::path path;
  std::size_t line = 0;  // 0-based buffer line
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

  // Rebuild the prebuilt row list from the current line breakpoints + advertised
  // filters. Cheap (breakpoint counts are small); called on any input change.
  void Rebuild(const editor::BreakpointStore& breakpoints);

  const std::vector<DebugBreakpointRowView>& Rows() const { return rows_; }
  std::size_t RowCount() const { return rows_.size(); }

 private:
  std::vector<std::string> enabled_filter_ids_;
  bool seeded_ = false;
  std::vector<dap_protocol::DapExceptionFilter> advertised_;
  std::vector<DebugBreakpointRowView> rows_;
};

}  // namespace microide::workspace
