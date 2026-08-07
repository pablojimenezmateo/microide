#include "workspace/debug/DebugBreakpointsModel.h"

#include <algorithm>
#include <utility>

namespace microide::workspace {

namespace {

// DAP distinguishes two unverified states, and conflating them is a UI lie.
// `Breakpoint.reason` is "pending" — "might be verified in the future, but the
// adapter cannot verify it in the current state" — or "failed" — "not able to be
// verified, and the adapter does not believe it can be verified without
// intervention". Only the latter is a failure.
//
// This matters for the most ordinary debugging flow there is. gdb answers
// `setBreakpoints` sent BEFORE launch with `verified:false, reason:"pending"`
// (symbols are not loaded yet), and that breakpoint then binds and hits — verified
// against gdb 17.2, which reports `hitBreakpointIds` for exactly such a
// breakpoint. Painting it in the warning tint made every pre-launch gdb breakpoint
// look broken while working perfectly.
bool UnverifiedReasonIsFailure(std::string_view reason) {
  return reason != "pending";
}

}  // namespace


namespace {

bool Contains(const std::vector<std::string>& ids, const std::string& id) {
  return std::find(ids.begin(), ids.end(), id) != ids.end();
}

}  // namespace

void DebugBreakpointsModel::SetEnabledFilterIds(std::vector<std::string> ids, bool seeded) {
  enabled_filter_ids_ = std::move(ids);
  seeded_ = seeded;
}

bool DebugBreakpointsModel::SetAdvertisedFilters(
    const std::vector<dap_protocol::DapExceptionFilter>& filters) {
  advertised_ = filters;
  if (seeded_) {
    return false;
  }
  // First time filters are seen for this project: adopt the adapter defaults.
  seeded_ = true;
  std::vector<std::string> defaults;
  for (const dap_protocol::DapExceptionFilter& filter : advertised_) {
    if (filter.default_enabled) {
      defaults.push_back(filter.filter);
    }
  }
  const bool changed = defaults != enabled_filter_ids_;
  enabled_filter_ids_ = std::move(defaults);
  return changed;
}

void DebugBreakpointsModel::ClearAdvertisedFilters() { advertised_.clear(); }

bool DebugBreakpointsModel::ToggleFilter(const std::string& filter_id) {
  const bool advertised =
      std::any_of(advertised_.begin(), advertised_.end(),
                  [&](const dap_protocol::DapExceptionFilter& f) { return f.filter == filter_id; });
  if (!advertised) {
    return false;
  }
  auto it = std::find(enabled_filter_ids_.begin(), enabled_filter_ids_.end(), filter_id);
  if (it != enabled_filter_ids_.end()) {
    enabled_filter_ids_.erase(it);
  } else {
    enabled_filter_ids_.push_back(filter_id);
  }
  return true;
}

bool DebugBreakpointsModel::IsEnabled(const std::string& filter_id) const {
  return Contains(enabled_filter_ids_, filter_id);
}

std::vector<std::string> DebugBreakpointsModel::EnabledAdvertisedIds() const {
  std::vector<std::string> ids;
  for (const dap_protocol::DapExceptionFilter& filter : advertised_) {
    if (Contains(enabled_filter_ids_, filter.filter)) {
      ids.push_back(filter.filter);
    }
  }
  return ids;
}

void DebugBreakpointsModel::SetFilterConditions(std::map<std::string, std::string> conditions) {
  filter_conditions_ = std::move(conditions);
}

bool DebugBreakpointsModel::SetFilterCondition(const std::string& filter_id,
                                               std::optional<std::string> condition) {
  const bool advertised =
      std::any_of(advertised_.begin(), advertised_.end(),
                  [&](const dap_protocol::DapExceptionFilter& f) { return f.filter == filter_id; });
  if (!advertised) {
    return false;
  }
  if (!condition || condition->empty()) {
    return filter_conditions_.erase(filter_id) > 0;
  }
  auto [it, inserted] = filter_conditions_.try_emplace(filter_id, *condition);
  if (!inserted) {
    if (it->second == *condition) {
      return false;
    }
    it->second = *condition;
  }
  return true;
}

std::vector<std::pair<std::string, std::string>> DebugBreakpointsModel::EnabledFilterOptions()
    const {
  std::vector<std::pair<std::string, std::string>> options;
  for (const dap_protocol::DapExceptionFilter& filter : advertised_) {
    if (!Contains(enabled_filter_ids_, filter.filter)) {
      continue;
    }
    const auto it = filter_conditions_.find(filter.filter);
    const std::string condition = it != filter_conditions_.end() ? it->second : std::string{};
    options.emplace_back(filter.filter, condition);
  }
  return options;
}

void DebugBreakpointsModel::Rebuild(
    const editor::BreakpointStore& breakpoints,
    const editor::FunctionBreakpointStore& function_breakpoints) {
  // Rows are OVERWRITTEN in place rather than cleared and re-pushed. Every row
  // owns four strings and a std::filesystem::path, and clearing frees all of them
  // so the next rebuild has to allocate them again -- 500 breakpoints across 20
  // files cost ~500 path allocations a pass, on a list that is unchanged between
  // most rebuilds. `kEmptyRow` is COPY-assigned (never moved from a temporary):
  // copy-assigning an empty string or path clears the target and keeps its
  // capacity, which is the whole point (TD-2026-08-06-159).
  static const DebugBreakpointRowView kEmptyRow{};
  std::size_t out = 0;
  const auto next_row = [&]() -> DebugBreakpointRowView& {
    if (out == rows_.size()) {
      rows_.emplace_back();
    }
    DebugBreakpointRowView& row = rows_[out++];
    row = kEmptyRow;
    return row;
  };

  if (!advertised_.empty()) {
    DebugBreakpointRowView& header = next_row();
    header.kind = DebugBreakpointRowView::Kind::Header;
    header.display = "Exception Breakpoints";
    for (const dap_protocol::DapExceptionFilter& filter : advertised_) {
      DebugBreakpointRowView& row = next_row();
      row.kind = DebugBreakpointRowView::Kind::ExceptionFilter;
      row.display = filter.label;
      row.filter_id = filter.filter;
      row.supports_condition = filter.supports_condition;
      row.enabled = Contains(enabled_filter_ids_, filter.filter);
      if (const auto it = filter_conditions_.find(filter.filter);
          it != filter_conditions_.end() && !it->second.empty()) {
        row.secondary = "when " + it->second;
      }
    }
  }

  const std::vector<editor::FunctionBreakpoint>& functions = function_breakpoints.All();
  if (!functions.empty()) {
    DebugBreakpointRowView& header = next_row();
    header.kind = DebugBreakpointRowView::Kind::Header;
    header.display = "Function Breakpoints";
    for (std::size_t i = 0; i < functions.size(); ++i) {
      const editor::FunctionBreakpoint& fn = functions[i];
      DebugBreakpointRowView& row = next_row();
      row.kind = DebugBreakpointRowView::Kind::FunctionBreakpoint;
      row.display = fn.name;
      row.function_name = fn.name;
      row.function_index = i;
      row.enabled = fn.enabled;
      if (fn.condition && !fn.condition->empty()) {
        row.secondary = "when " + *fn.condition;
      } else if (fn.hit_condition && !fn.hit_condition->empty()) {
        row.secondary = "hits " + *fn.hit_condition;
      }
      const bool adapter_responded =
          fn.verified || fn.adapter_id != 0 || !fn.verify_message.empty();
      if (adapter_responded && !fn.verified) {
        const std::string reason =
            !fn.verify_message.empty() ? fn.verify_message : "unverified";
        // Still surface the reason in the muted trailer either way — the user
        // should see "pending" — but only tint the row as a failure when the
        // adapter says it genuinely could not bind it.
        row.failed = UnverifiedReasonIsFailure(reason);
        row.secondary = row.secondary.empty() ? reason : (row.secondary + " — " + reason);
      }
    }
  }

  // Views, not SnapshotAll: this pass reads each file's breakpoints once and keeps
  // nothing, so the owning snapshot's deep copy of every file's whole vector was
  // 88 KB of memcpy per rebuild for nothing.
  breakpoints.FillSortedFileViews(&file_views_scratch_);
  if (!file_views_scratch_.empty()) {
    DebugBreakpointRowView& header = next_row();
    header.kind = DebugBreakpointRowView::Kind::Header;
    header.display = "Breakpoints";
    for (const editor::BreakpointStore::FileBreakpointsView& file : file_views_scratch_) {
      const std::string filename = file.path->filename().string();
      for (const editor::Breakpoint& breakpoint : file.breakpoints) {
        DebugBreakpointRowView& row = next_row();
        row.kind = DebugBreakpointRowView::Kind::Breakpoint;
        // 1-based line for display; the buffer index stays 0-based for nav.
        row.display = filename + ':' + std::to_string(breakpoint.line + 1);
        if (breakpoint.condition && !breakpoint.condition->empty()) {
          row.secondary = "when " + *breakpoint.condition;
        } else if (breakpoint.log_message && !breakpoint.log_message->empty()) {
          row.secondary = "log";
        } else if (breakpoint.hit_condition && !breakpoint.hit_condition->empty()) {
          row.secondary = "hits " + *breakpoint.hit_condition;
        }
        // Enabled state is shown by the row's checkbox (set below), not the trailer.
        row.enabled = breakpoint.enabled;
        // Surface adapter verification: when the adapter has responded for this
        // breakpoint but did not bind it, mark it failed and show why (so the user
        // is not left with a silently dimmed dot). "Responded" = verified, an
        // assigned adapter id, or a message — never true before a session binds.
        const bool adapter_responded = breakpoint.verified || breakpoint.adapter_id != 0 ||
                                       !breakpoint.verify_message.empty();
        if (adapter_responded && !breakpoint.verified) {
          const std::string reason =
              !breakpoint.verify_message.empty() ? breakpoint.verify_message : "unverified";
          row.failed = UnverifiedReasonIsFailure(reason);
          row.secondary = row.secondary.empty() ? reason : (row.secondary + " — " + reason);
        }
        row.path = *file.path;
        row.line = breakpoint.line;
      }
    }
  }
  // The only place capacity is given back: anything past the last row this pass
  // produced is destroyed, so a shrinking list does not keep rows it no longer shows.
  rows_.resize(out);
}

}  // namespace microide::workspace
