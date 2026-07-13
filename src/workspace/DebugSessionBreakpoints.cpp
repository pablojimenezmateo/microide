// Breakpoint / exception-filter send paths for DebugSession, split out of
// DebugSession.cpp to keep that translation unit thin (mirrors the
// DebugService / DebugServiceBreakpoints split). These are still DebugSession
// members; only their definitions live here. The send order (line breakpoints,
// function breakpoints, exception filters) is driven from HandleEvent's
// `initialized` handler in DebugSession.cpp.
#include "workspace/DebugSession.h"

#include <algorithm>
#include <filesystem>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "util/JsonValue.h"

namespace microide::workspace {

void DebugSession::SendAllBreakpoints() {
  if (!callbacks_.breakpoint_provider) {
    return;
  }
  for (const auto& file : callbacks_.breakpoint_provider()) {
    SendBreakpointsForFile(file);
  }
}

void DebugSession::SendBreakpointsForFile(const editor::BreakpointStore::FileBreakpoints& file) {
  const std::string source_path = file.path.generic_string();
  if (source_path.empty()) {
    return;
  }
  const dap_protocol::DapCapabilities& caps = client_->Capabilities();
  std::vector<dap_protocol::SetBreakpointInput> inputs;
  inputs.reserve(file.breakpoints.size());
  for (const editor::Breakpoint& breakpoint : file.breakpoints) {
    if (!breakpoint.enabled) {
      continue;
    }
    dap_protocol::SetBreakpointInput input;
    // BreakpointStore stores 0-based buffer lines; DAP wants 1-based. Clamp before
    // the narrowing cast so a forged/corrupt persisted breakpoint near INT_MAX /
    // SIZE_MAX cannot wrap to a negative or unrelated DAP line.
    input.line = breakpoint.line >= static_cast<std::size_t>(std::numeric_limits<int>::max() - 1)
                     ? std::numeric_limits<int>::max()
                     : static_cast<int>(breakpoint.line) + 1;
    // Phase 6 fields, gated on adapter capabilities so we never send a key an
    // adapter rejects. Empty values are omitted by the encoder regardless.
    if (caps.supports_conditional_breakpoints && breakpoint.condition) {
      input.condition = *breakpoint.condition;
    }
    if (caps.supports_hit_conditional_breakpoints && breakpoint.hit_condition) {
      input.hit_condition = *breakpoint.hit_condition;
    }
    if (caps.supports_log_points && breakpoint.log_message) {
      input.log_message = *breakpoint.log_message;
    }
    inputs.push_back(std::move(input));
  }

  const std::filesystem::path path = file.path;
  // Capture the requested 1-based lines (send order). The setBreakpoints response
  // is positional to this list, so the host matches each result to a breakpoint by
  // its requested line — robust to the user toggling another breakpoint in the same
  // file while this request is in flight.
  std::vector<int> requested_lines;
  requested_lines.reserve(inputs.size());
  for (const dap_protocol::SetBreakpointInput& input : inputs) {
    requested_lines.push_back(input.line);
  }
  client_->SendRequestAsync(
      "setBreakpoints", dap_protocol::MakeSetBreakpointsArguments(source_path, inputs),
      [this, path, requested_lines = std::move(requested_lines)](
          const dap_protocol::DapResponse& response) {
        if (response.success && callbacks_.on_breakpoints_verified) {
          callbacks_.on_breakpoints_verified(path, requested_lines,
                                             dap_protocol::ParseBreakpoints(response.body));
        }
      });
}

void DebugSession::ResendBreakpointsForFile(const std::filesystem::path& path) {
  if (!client_->IsInitialized() || !callbacks_.breakpoint_provider) {
    return;
  }
  const std::string target_key = path.lexically_normal().generic_string();
  for (const auto& file : callbacks_.breakpoint_provider()) {
    if (file.path.lexically_normal().generic_string() == target_key) {
      SendBreakpointsForFile(file);
      return;
    }
  }
  // No breakpoints remain for the file: send an empty list to clear them.
  SendBreakpointsForFile(editor::BreakpointStore::FileBreakpoints{.path = path});
}

void DebugSession::SendFunctionBreakpoints() {
  const dap_protocol::DapCapabilities& caps = client_->Capabilities();
  if (!caps.supports_function_breakpoints || !callbacks_.function_breakpoint_provider) {
    return;
  }
  std::vector<dap_protocol::SetFunctionBreakpointInput> inputs;
  std::vector<std::string> requested_names;
  for (const editor::FunctionBreakpoint& bp : callbacks_.function_breakpoint_provider()) {
    if (!bp.enabled || bp.name.empty()) {
      continue;
    }
    dap_protocol::SetFunctionBreakpointInput input;
    input.name = bp.name;
    if (caps.supports_conditional_breakpoints && bp.condition) {
      input.condition = *bp.condition;
    }
    if (caps.supports_hit_conditional_breakpoints && bp.hit_condition) {
      input.hit_condition = *bp.hit_condition;
    }
    requested_names.push_back(bp.name);
    inputs.push_back(std::move(input));
  }
  // Always send (even an empty list) so a live re-send after removing the last
  // function breakpoint clears them on the adapter. The response is positional to
  // `requested_names`; the host matches each result back to a breakpoint by name.
  client_->SendRequestAsync(
      "setFunctionBreakpoints", dap_protocol::MakeSetFunctionBreakpointsArguments(inputs),
      [this, requested_names = std::move(requested_names)](
          const dap_protocol::DapResponse& response) {
        if (response.success && callbacks_.on_function_breakpoints_verified) {
          callbacks_.on_function_breakpoints_verified(requested_names,
                                                      dap_protocol::ParseBreakpoints(response.body));
        }
      });
}

void DebugSession::ResendFunctionBreakpoints() {
  if (!client_->IsInitialized()) {
    return;
  }
  SendFunctionBreakpoints();
}

void DebugSession::SendExceptionFilters() {
  const dap_protocol::DapCapabilities& caps = client_->Capabilities();
  const std::vector<dap_protocol::DapExceptionFilter>& advertised = caps.exception_filters;
  if (advertised.empty() || !callbacks_.exception_filter_provider) {
    return;
  }
  const std::vector<ExceptionFilterRequest> enabled = callbacks_.exception_filter_provider();
  // Send only ids the adapter advertised, in advertised order, so we never push a
  // filter id the adapter would reject. A condition is honored only when both the
  // adapter (supportsExceptionFilterOptions) and the specific filter
  // (supportsCondition) accept it; otherwise the filter is sent plain.
  std::vector<std::string> ids;
  std::vector<std::pair<std::string, std::string>> options;
  for (const dap_protocol::DapExceptionFilter& filter : advertised) {
    const auto it = std::find_if(
        enabled.begin(), enabled.end(),
        [&filter](const ExceptionFilterRequest& req) { return req.id == filter.filter; });
    if (it == enabled.end()) {
      continue;
    }
    if (caps.supports_exception_filter_options && filter.supports_condition &&
        !it->condition.empty()) {
      options.emplace_back(filter.filter, it->condition);
    } else {
      ids.push_back(filter.filter);
    }
  }
  const util::JsonValue arguments =
      options.empty() ? dap_protocol::MakeSetExceptionBreakpointsArguments(ids)
                      : dap_protocol::MakeSetExceptionBreakpointsArguments(ids, options);
  client_->SendRequestAsync("setExceptionBreakpoints", arguments, {});
}

void DebugSession::ResendExceptionFilters() {
  if (!client_->IsInitialized()) {
    return;
  }
  SendExceptionFilters();
}

}  // namespace microide::workspace
