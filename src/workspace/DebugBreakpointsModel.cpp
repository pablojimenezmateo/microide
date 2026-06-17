#include "workspace/DebugBreakpointsModel.h"

#include <algorithm>
#include <utility>

namespace microide::workspace {

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

void DebugBreakpointsModel::Rebuild(const editor::BreakpointStore& breakpoints) {
  rows_.clear();

  if (!advertised_.empty()) {
    DebugBreakpointRowView header;
    header.kind = DebugBreakpointRowView::Kind::Header;
    header.display = "Exception Breakpoints";
    rows_.push_back(std::move(header));
    for (const dap_protocol::DapExceptionFilter& filter : advertised_) {
      DebugBreakpointRowView row;
      row.kind = DebugBreakpointRowView::Kind::ExceptionFilter;
      row.display = filter.label;
      row.filter_id = filter.filter;
      row.enabled = Contains(enabled_filter_ids_, filter.filter);
      rows_.push_back(std::move(row));
    }
  }

  const std::vector<editor::BreakpointStore::FileBreakpoints> files = breakpoints.SnapshotAll();
  if (!files.empty()) {
    DebugBreakpointRowView header;
    header.kind = DebugBreakpointRowView::Kind::Header;
    header.display = "Breakpoints";
    rows_.push_back(std::move(header));
    for (const editor::BreakpointStore::FileBreakpoints& file : files) {
      const std::string filename = file.path.filename().string();
      for (const editor::Breakpoint& breakpoint : file.breakpoints) {
        DebugBreakpointRowView row;
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
        if (!breakpoint.enabled) {
          row.secondary = row.secondary.empty() ? "disabled" : (row.secondary + " (disabled)");
        }
        row.path = file.path;
        row.line = breakpoint.line;
        rows_.push_back(std::move(row));
      }
    }
  }
}

}  // namespace microide::workspace
