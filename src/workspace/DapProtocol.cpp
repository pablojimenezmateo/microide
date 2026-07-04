#include "workspace/DapProtocol.h"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace microide::workspace::dap_protocol {

using util::JsonArray;
using util::JsonObject;
using util::JsonValue;

namespace {

int AsInt(const JsonValue& value, int fallback = 0) {
  return static_cast<int>(value.AsInt(fallback));
}

// JsonValue::AsString() already returns a static empty string for non-string
// values, so return its reference directly — no per-call copy across the ~29
// Parse* field extractions (the field assignment at the call site copies once).
const std::string& AsString(const JsonValue& value) { return value.AsString(); }

constexpr std::size_t kMaxDapListEntries = 10000;

}  // namespace

JsonValue MakeSetBreakpointsArguments(const std::string& source_path,
                                      const std::vector<SetBreakpointInput>& breakpoints) {
  JsonObject source;
  source["path"] = JsonValue(source_path);

  JsonArray entries;
  entries.reserve(breakpoints.size());
  for (const SetBreakpointInput& breakpoint : breakpoints) {
    JsonObject entry;
    entry["line"] = JsonValue(static_cast<std::int64_t>(breakpoint.line));
    if (!breakpoint.condition.empty()) {
      entry["condition"] = JsonValue(breakpoint.condition);
    }
    if (!breakpoint.hit_condition.empty()) {
      entry["hitCondition"] = JsonValue(breakpoint.hit_condition);
    }
    if (!breakpoint.log_message.empty()) {
      entry["logMessage"] = JsonValue(breakpoint.log_message);
    }
    entries.push_back(JsonValue(std::move(entry)));
  }

  JsonObject args;
  args["source"] = JsonValue(std::move(source));
  args["breakpoints"] = JsonValue(std::move(entries));
  return JsonValue(std::move(args));
}

JsonValue MakeSetExceptionBreakpointsArguments(const std::vector<std::string>& filter_ids) {
  JsonArray filters;
  filters.reserve(filter_ids.size());
  for (const std::string& id : filter_ids) {
    filters.push_back(JsonValue(id));
  }
  JsonObject args;
  args["filters"] = JsonValue(std::move(filters));
  return JsonValue(std::move(args));
}

JsonValue MakeSetExceptionBreakpointsArguments(
    const std::vector<std::string>& plain_filter_ids,
    const std::vector<std::pair<std::string, std::string>>& filter_options) {
  JsonArray filters;
  filters.reserve(plain_filter_ids.size());
  for (const std::string& id : plain_filter_ids) {
    filters.push_back(JsonValue(id));
  }
  JsonObject args;
  args["filters"] = JsonValue(std::move(filters));
  if (!filter_options.empty()) {
    JsonArray options;
    options.reserve(filter_options.size());
    for (const auto& [filter_id, condition] : filter_options) {
      JsonObject option;
      option["filterId"] = JsonValue(filter_id);
      if (!condition.empty()) {
        option["condition"] = JsonValue(condition);
      }
      options.push_back(JsonValue(std::move(option)));
    }
    args["filterOptions"] = JsonValue(std::move(options));
  }
  return JsonValue(std::move(args));
}

JsonValue MakeSetFunctionBreakpointsArguments(
    const std::vector<SetFunctionBreakpointInput>& breakpoints) {
  JsonArray entries;
  entries.reserve(breakpoints.size());
  for (const SetFunctionBreakpointInput& breakpoint : breakpoints) {
    JsonObject entry;
    entry["name"] = JsonValue(breakpoint.name);
    if (!breakpoint.condition.empty()) {
      entry["condition"] = JsonValue(breakpoint.condition);
    }
    if (!breakpoint.hit_condition.empty()) {
      entry["hitCondition"] = JsonValue(breakpoint.hit_condition);
    }
    entries.push_back(JsonValue(std::move(entry)));
  }
  JsonObject args;
  args["breakpoints"] = JsonValue(std::move(entries));
  return JsonValue(std::move(args));
}

JsonValue MakeStackTraceArguments(int thread_id, int start_frame, int levels) {
  JsonObject args;
  args["threadId"] = JsonValue(static_cast<std::int64_t>(thread_id));
  args["startFrame"] = JsonValue(static_cast<std::int64_t>(start_frame));
  args["levels"] = JsonValue(static_cast<std::int64_t>(levels));
  return JsonValue(std::move(args));
}

JsonValue MakeScopesArguments(int frame_id) {
  JsonObject args;
  args["frameId"] = JsonValue(static_cast<std::int64_t>(frame_id));
  return JsonValue(std::move(args));
}

JsonValue MakeVariablesArguments(int variables_reference, int start, int count) {
  JsonObject args;
  args["variablesReference"] = JsonValue(static_cast<std::int64_t>(variables_reference));
  if (start != 0) {
    args["start"] = JsonValue(static_cast<std::int64_t>(start));
  }
  if (count != 0) {
    args["count"] = JsonValue(static_cast<std::int64_t>(count));
  }
  return JsonValue(std::move(args));
}

JsonValue MakeSetVariableArguments(const SetVariableInput& input) {
  JsonObject args;
  args["variablesReference"] = JsonValue(static_cast<std::int64_t>(input.variables_reference));
  args["name"] = JsonValue(input.name);
  args["value"] = JsonValue(input.value);
  return JsonValue(std::move(args));
}

JsonValue MakeEvaluateArguments(const std::string& expression, int frame_id,
                                const std::string& context) {
  JsonObject args;
  args["expression"] = JsonValue(expression);
  // gdb numbers the *top* stack frame as id 0, so frame 0 is a real, evaluable
  // frame — omitting its frameId makes gdb evaluate locals in global scope ("No
  // symbol in current context"). Only a negative id means "no frame" (e.g.
  // pre-launch REPL settings, or evaluate while running): then omit frameId.
  if (frame_id >= 0) {
    args["frameId"] = JsonValue(static_cast<std::int64_t>(frame_id));
  }
  if (!context.empty()) {
    args["context"] = JsonValue(context);
  }
  return JsonValue(std::move(args));
}

JsonValue MakeRequest(int seq, const std::string& command, const JsonValue& arguments) {
  JsonObject req;
  req["seq"] = JsonValue(static_cast<std::int64_t>(seq));
  req["type"] = JsonValue("request");
  req["command"] = JsonValue(command);
  if (!arguments.IsNull()) {
    req["arguments"] = arguments;
  }
  return JsonValue(std::move(req));
}

JsonValue MakeResponse(int seq, int request_seq, const std::string& command, bool success,
                       const std::string& message, JsonValue body) {
  JsonObject resp;
  resp["seq"] = JsonValue(static_cast<std::int64_t>(seq));
  resp["type"] = JsonValue("response");
  resp["request_seq"] = JsonValue(static_cast<std::int64_t>(request_seq));
  resp["command"] = JsonValue(command);
  resp["success"] = JsonValue(success);
  if (!message.empty()) {
    resp["message"] = JsonValue(message);
  }
  if (!body.IsNull()) {
    resp["body"] = std::move(body);
  }
  return JsonValue(std::move(resp));
}

DapResponse ParseResponse(const JsonValue& msg) {
  DapResponse response;
  response.success = msg["success"].AsBool(false);
  response.command = AsString(msg["command"]);
  response.message = AsString(msg["message"]);
  response.body = msg["body"];
  return response;
}

DapCapabilities ParseCapabilities(const JsonValue& body) {
  DapCapabilities caps;
  caps.supports_configuration_done_request =
      body["supportsConfigurationDoneRequest"].AsBool(false);
  caps.supports_conditional_breakpoints = body["supportsConditionalBreakpoints"].AsBool(false);
  caps.supports_hit_conditional_breakpoints =
      body["supportsHitConditionalBreakpoints"].AsBool(false);
  caps.supports_log_points = body["supportsLogPoints"].AsBool(false);
  caps.supports_function_breakpoints = body["supportsFunctionBreakpoints"].AsBool(false);
  caps.supports_set_variable = body["supportsSetVariable"].AsBool(false);
  caps.supports_evaluate_for_hovers = body["supportsEvaluateForHovers"].AsBool(false);
  caps.supports_terminate_request = body["supportsTerminateRequest"].AsBool(false);
  caps.supports_restart_request = body["supportsRestartRequest"].AsBool(false);
  caps.supports_step_back = body["supportsStepBack"].AsBool(false);
  caps.supports_exception_filter_options = body["supportsExceptionFilterOptions"].AsBool(false);
  const auto& filters = body["exceptionBreakpointFilters"].AsArray();
  const std::size_t filter_count = std::min(filters.size(), kMaxDapListEntries);
  caps.exception_filters.reserve(filter_count);
  for (std::size_t i = 0; i < filter_count; ++i) {
    const JsonValue& item = filters[i];
    DapExceptionFilter filter;
    filter.filter = AsString(item["filter"]);
    if (filter.filter.empty()) {
      continue;
    }
    filter.label = AsString(item["label"]);
    if (filter.label.empty()) {
      filter.label = filter.filter;
    }
    filter.description = AsString(item["description"]);
    filter.default_enabled = item["default"].AsBool(false);
    filter.supports_condition = item["supportsCondition"].AsBool(false);
    caps.exception_filters.push_back(std::move(filter));
  }
  return caps;
}

void MergeCapabilities(DapCapabilities& caps, const JsonValue& body) {
  // Presence-checked overlay: only fields the body carries are updated. Keep this
  // list aligned with ParseCapabilities above.
  if (body.HasKey("supportsConfigurationDoneRequest"))
    caps.supports_configuration_done_request = body["supportsConfigurationDoneRequest"].AsBool();
  if (body.HasKey("supportsConditionalBreakpoints"))
    caps.supports_conditional_breakpoints = body["supportsConditionalBreakpoints"].AsBool();
  if (body.HasKey("supportsHitConditionalBreakpoints"))
    caps.supports_hit_conditional_breakpoints = body["supportsHitConditionalBreakpoints"].AsBool();
  if (body.HasKey("supportsLogPoints"))
    caps.supports_log_points = body["supportsLogPoints"].AsBool();
  if (body.HasKey("supportsFunctionBreakpoints"))
    caps.supports_function_breakpoints = body["supportsFunctionBreakpoints"].AsBool();
  if (body.HasKey("supportsSetVariable"))
    caps.supports_set_variable = body["supportsSetVariable"].AsBool();
  if (body.HasKey("supportsEvaluateForHovers"))
    caps.supports_evaluate_for_hovers = body["supportsEvaluateForHovers"].AsBool();
  if (body.HasKey("supportsTerminateRequest"))
    caps.supports_terminate_request = body["supportsTerminateRequest"].AsBool();
  if (body.HasKey("supportsRestartRequest"))
    caps.supports_restart_request = body["supportsRestartRequest"].AsBool();
  if (body.HasKey("supportsStepBack"))
    caps.supports_step_back = body["supportsStepBack"].AsBool();
  if (body.HasKey("supportsExceptionFilterOptions"))
    caps.supports_exception_filter_options = body["supportsExceptionFilterOptions"].AsBool();
  // The exception-filter list is replaced wholesale only when the body restates it
  // (an adapter that re-advertises filters sends the full array, not a delta).
  if (body.HasKey("exceptionBreakpointFilters"))
    caps.exception_filters = ParseCapabilities(body).exception_filters;
}

DapSource ParseSource(const JsonValue& value) {
  DapSource source;
  source.name = AsString(value["name"]);
  source.path = AsString(value["path"]);
  source.source_reference = AsInt(value["sourceReference"]);
  return source;
}

DapThread ParseThread(const JsonValue& value) {
  DapThread thread;
  thread.id = AsInt(value["id"]);
  thread.name = AsString(value["name"]);
  return thread;
}

std::vector<DapThread> ParseThreads(const JsonValue& body) {
  std::vector<DapThread> threads;
  const auto& array = body["threads"].AsArray();
  // Cap like ParseStackFrames: a hostile/buggy adapter can pack millions of
  // entries into a sub-64 MiB frame; each becomes strings materialized on the
  // main thread (and threads feed a UI picker). 10000 is far beyond any real
  // process's thread count.
  const std::size_t count = std::min(array.size(), kMaxDapListEntries);
  threads.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    threads.push_back(ParseThread(array[i]));
  }
  return threads;
}

DapStackFrame ParseStackFrame(const JsonValue& value) {
  DapStackFrame frame;
  frame.id = AsInt(value["id"]);
  frame.name = AsString(value["name"]);
  frame.source = ParseSource(value["source"]);
  frame.line = AsInt(value["line"]);
  frame.column = AsInt(value["column"]);
  frame.presentation_hint = AsString(value["presentationHint"]);
  return frame;
}

std::vector<DapStackFrame> ParseStackFrames(const JsonValue& body) {
  std::vector<DapStackFrame> frames;
  const auto& array = body["stackFrames"].AsArray();
  // Cap frame count. A hostile/buggy adapter can ignore our `levels` request cap
  // and return a 64 MiB array of frames; each frame is materialized into a
  // filesystem::path + display strings on the main thread (BuildExecutionView),
  // so an uncapped array is a UI-thread stall + heap spike. 10000 frames is far
  // beyond any real call stack a human would page through.
  const std::size_t count = std::min(array.size(), kMaxDapListEntries);
  frames.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    frames.push_back(ParseStackFrame(array[i]));
  }
  return frames;
}

DapScope ParseScope(const JsonValue& value) {
  DapScope scope;
  scope.name = AsString(value["name"]);
  scope.variables_reference = AsInt(value["variablesReference"]);
  scope.expensive = value["expensive"].AsBool(false);
  scope.presentation_hint = AsString(value["presentationHint"]);
  scope.named_variables = AsInt(value["namedVariables"]);
  scope.indexed_variables = AsInt(value["indexedVariables"]);
  scope.count_reported = value.HasKey("namedVariables") || value.HasKey("indexedVariables");
  return scope;
}

std::vector<DapScope> ParseScopes(const JsonValue& body) {
  std::vector<DapScope> scopes;
  const auto& array = body["scopes"].AsArray();
  // Cap like ParseStackFrames; a frame has only a handful of real scopes.
  const std::size_t count = std::min(array.size(), kMaxDapListEntries);
  scopes.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    scopes.push_back(ParseScope(array[i]));
  }
  return scopes;
}

DapVariable ParseVariable(const JsonValue& value) {
  DapVariable variable;
  variable.name = AsString(value["name"]);
  variable.value = AsString(value["value"]);
  variable.type = AsString(value["type"]);
  variable.evaluate_name = AsString(value["evaluateName"]);
  variable.variables_reference = AsInt(value["variablesReference"]);
  variable.named_variables = AsInt(value["namedVariables"]);
  variable.indexed_variables = AsInt(value["indexedVariables"]);
  variable.count_reported = value.HasKey("namedVariables") || value.HasKey("indexedVariables");
  return variable;
}

std::vector<DapVariable> ParseVariables(const JsonValue& body) {
  std::vector<DapVariable> variables;
  const auto& array = body["variables"].AsArray();
  // Cap like ParseStackFrames. This is the largest and most frequent DAP array
  // (container expansion): the adapter only treats our requested `count` as a
  // hint, so an uncapped reply packs millions of tiny objects into a sub-64 MiB
  // frame, each becoming four strings on the main thread plus a tree Node
  // downstream. 10000 is far beyond what a user pages through in one node.
  const std::size_t count = std::min(array.size(), kMaxDapListEntries);
  variables.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    variables.push_back(ParseVariable(array[i]));
  }
  return variables;
}

DapBreakpoint ParseBreakpoint(const JsonValue& value) {
  DapBreakpoint breakpoint;
  breakpoint.id = AsInt(value["id"]);
  breakpoint.verified = value["verified"].AsBool(false);
  breakpoint.message = AsString(value["message"]);
  // gdb 17.2 omits `message` and instead reports `reason` ("pending"/"failed") for
  // an unverified breakpoint; surface that as the message so the panel has a reason.
  if (breakpoint.message.empty()) {
    breakpoint.message = AsString(value["reason"]);
  }
  breakpoint.line = AsInt(value["line"]);
  breakpoint.column = AsInt(value["column"]);
  return breakpoint;
}

std::vector<DapBreakpoint> ParseBreakpoints(const JsonValue& body) {
  std::vector<DapBreakpoint> breakpoints;
  const auto& array = body["breakpoints"].AsArray();
  // Cap like ParseStackFrames; a setBreakpoints reply mirrors the breakpoints we
  // sent, so a flood beyond this is adversarial.
  const std::size_t count = std::min(array.size(), kMaxDapListEntries);
  breakpoints.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    breakpoints.push_back(ParseBreakpoint(array[i]));
  }
  return breakpoints;
}

DapStoppedEvent ParseStoppedEvent(const JsonValue& body) {
  DapStoppedEvent event;
  event.reason = AsString(body["reason"]);
  event.description = AsString(body["description"]);
  event.text = AsString(body["text"]);
  event.thread_id = AsInt(body["threadId"]);
  event.all_threads_stopped = body["allThreadsStopped"].AsBool(false);
  const auto& hit_ids = body["hitBreakpointIds"].AsArray();
  const std::size_t hit_count = std::min(hit_ids.size(), kMaxDapListEntries);
  event.hit_breakpoint_ids.reserve(hit_count);
  for (std::size_t i = 0; i < hit_count; ++i) {
    event.hit_breakpoint_ids.push_back(AsInt(hit_ids[i]));
  }
  return event;
}

DapOutputEvent ParseOutputEvent(const JsonValue& body) {
  DapOutputEvent event;
  event.category = AsString(body["category"]);
  if (event.category.empty()) {
    event.category = "console";
  }
  event.output = AsString(body["output"]);
  event.variables_reference = AsInt(body["variablesReference"]);
  return event;
}

DapEvaluateResult ParseEvaluateResult(const JsonValue& body) {
  DapEvaluateResult result;
  result.result = AsString(body["result"]);
  result.type = AsString(body["type"]);
  result.variables_reference = AsInt(body["variablesReference"]);
  return result;
}

DapSetVariableResult ParseSetVariableResult(const JsonValue& body) {
  DapSetVariableResult result;
  result.value = AsString(body["value"]);
  result.type = AsString(body["type"]);
  result.variables_reference = AsInt(body["variablesReference"]);
  result.named_variables = AsInt(body["namedVariables"]);
  result.indexed_variables = AsInt(body["indexedVariables"]);
  return result;
}

}  // namespace microide::workspace::dap_protocol
