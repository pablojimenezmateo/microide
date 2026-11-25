#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <string>
#include <utility>
#include <vector>

#include "util/JsonValue.h"

namespace microide::workspace::dap_protocol {

struct DapExceptionFilter {
  std::string filter;
  std::string label;
  std::string description;
  bool default_enabled = false;
  bool supports_condition = false;
};

struct DapCapabilities {
  bool supports_configuration_done_request = false;
  bool supports_conditional_breakpoints = false;
  bool supports_hit_conditional_breakpoints = false;
  bool supports_log_points = false;
  bool supports_function_breakpoints = false;
  bool supports_set_variable = false;
  bool supports_evaluate_for_hovers = false;
  bool supports_terminate_request = false;
  bool supports_restart_request = false;
  bool supports_step_back = false;
  bool supports_exception_filter_options = false;
  std::vector<DapExceptionFilter> exception_filters;
};

struct DapResponse {
  bool success = false;
  std::string command;
  std::string message;
  util::JsonValue body;
};

struct DapSource {
  std::string name;
  std::string path;
  int source_reference = 0;
};

struct DapThread {
  int id = 0;
  std::string name;
};

struct DapStackFrame {
  int id = 0;
  std::string name;
  DapSource source;
  int line = 0;
  int column = 0;
  std::string presentation_hint;
};

struct DapScope {
  std::string name;
  int variables_reference = 0;
  bool expensive = false;
  std::string presentation_hint;
  int named_variables = 0;
  int indexed_variables = 0;
  bool count_reported = false;
};

struct DapVariable {
  std::string name;
  std::string value;
  std::string type;
  std::string evaluate_name;
  int variables_reference = 0;
  int named_variables = 0;
  int indexed_variables = 0;
  bool count_reported = false;
};

struct DapBreakpoint {
  int id = 0;
  bool verified = false;
  std::string message;
  int line = 0;
  int column = 0;
};

struct DapStoppedEvent {
  std::string reason;
  std::string description;
  std::string text;
  std::optional<int> thread_id;
  bool all_threads_stopped = false;
  std::vector<int> hit_breakpoint_ids;
};

struct DapContinuedEvent {
  std::optional<int> thread_id;
  bool all_threads_continued = false;
};

struct DapOutputEvent {
  std::string category;
  std::string output;
  int variables_reference = 0;
};

struct DapEvaluateResult {
  std::string result;
  std::string type;
  int variables_reference = 0;
};

struct DapSetVariableResult {
  std::string value;
  std::string type;
  int variables_reference = 0;
  int named_variables = 0;
  int indexed_variables = 0;
};

struct SetBreakpointInput {
  int line = 0;
  std::string condition;
  std::string hit_condition;
  std::string log_message;
};

std::string AdapterHitCondition(std::string_view hit_condition, bool ignore_count_semantics);

util::JsonValue MakeSetBreakpointsArguments(const std::string& source_path,
                                            const std::vector<SetBreakpointInput>& breakpoints);

util::JsonValue MakeSetExceptionBreakpointsArguments(const std::vector<std::string>& filter_ids);

util::JsonValue MakeSetExceptionBreakpointsArguments(
    const std::vector<std::string>& plain_filter_ids,
    const std::vector<std::pair<std::string, std::string>>& filter_options);

struct SetFunctionBreakpointInput {
  std::string name;
  std::string condition;
  std::string hit_condition;
};

util::JsonValue MakeSetFunctionBreakpointsArguments(
    const std::vector<SetFunctionBreakpointInput>& breakpoints);

util::JsonValue MakeStackTraceArguments(int thread_id, int start_frame, int levels);
util::JsonValue MakeThreadIdArguments(int thread_id);
util::JsonValue MakeScopesArguments(int frame_id);
util::JsonValue MakeVariablesArguments(int variables_reference, int start, int count);

struct SetVariableInput {
  int variables_reference = 0;
  std::string name;
  std::string value;
};
util::JsonValue MakeSetVariableArguments(const SetVariableInput& input);

util::JsonValue MakeEvaluateArguments(const std::string& expression, int frame_id,
                                      const std::string& context);

util::JsonValue MakeRequest(int seq, const std::string& command, util::JsonValue arguments);
util::JsonValue MakeResponse(int seq, int request_seq, const std::string& command, bool success,
                             const std::string& message, util::JsonValue body);

DapResponse ParseResponse(const util::JsonValue& msg);
DapCapabilities ParseCapabilities(const util::JsonValue& body);

void MergeCapabilities(DapCapabilities& caps, const util::JsonValue& body);

DapSource ParseSource(const util::JsonValue& value);
DapThread ParseThread(const util::JsonValue& value);
std::vector<DapThread> ParseThreads(const util::JsonValue& body);
DapStackFrame ParseStackFrame(const util::JsonValue& value);
std::vector<DapStackFrame> ParseStackFrames(const util::JsonValue& body);
DapScope ParseScope(const util::JsonValue& value);
std::vector<DapScope> ParseScopes(const util::JsonValue& body);
DapVariable ParseVariable(const util::JsonValue& value);
std::vector<DapVariable> ParseVariables(const util::JsonValue& body);
DapBreakpoint ParseBreakpoint(const util::JsonValue& value);
std::vector<DapBreakpoint> ParseBreakpoints(const util::JsonValue& body);
DapStoppedEvent ParseStoppedEvent(const util::JsonValue& body);
DapContinuedEvent ParseContinuedEvent(const util::JsonValue& body);
DapOutputEvent ParseOutputEvent(const util::JsonValue& body);
DapEvaluateResult ParseEvaluateResult(const util::JsonValue& body);
DapSetVariableResult ParseSetVariableResult(const util::JsonValue& body);

}
