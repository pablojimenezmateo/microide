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

struct DebugBreakpointRowView {
  enum class Kind { Header, ExceptionFilter, Breakpoint, FunctionBreakpoint };
  Kind kind = Kind::Breakpoint;
  std::string display;
  std::string secondary;
  std::string filter_id;
  bool supports_condition = false;
  bool enabled = false;
  std::filesystem::path path;
  std::size_t line = 0;
  std::string function_name;
  std::size_t function_index = 0;
  bool failed = false;
};

class DebugBreakpointsModel {
 public:
  const std::vector<std::string>& EnabledFilterIds() const { return enabled_filter_ids_; }
  bool Seeded() const { return seeded_; }
  void SetEnabledFilterIds(std::vector<std::string> ids, bool seeded);

  bool SetAdvertisedFilters(const std::vector<dap_protocol::DapExceptionFilter>& filters);
  const std::vector<dap_protocol::DapExceptionFilter>& AdvertisedFilters() const {
    return advertised_;
  }

  bool ToggleFilter(const std::string& filter_id);
  bool IsEnabled(const std::string& filter_id) const;
  std::vector<std::string> EnabledAdvertisedIds() const;

  const std::map<std::string, std::string>& FilterConditions() const { return filter_conditions_; }
  void SetFilterConditions(std::map<std::string, std::string> conditions);
  bool SetFilterCondition(const std::string& filter_id, std::optional<std::string> condition);
  std::vector<std::pair<std::string, std::string>> EnabledFilterOptions() const;

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
  mutable std::vector<editor::BreakpointStore::FileBreakpointsView> file_views_scratch_;
};

}
