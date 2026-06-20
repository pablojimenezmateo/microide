#include "TestSupport.h"

#include "util/JsonValue.h"
#include "workspace/DapProtocol.h"

#include <optional>

namespace microide::tests {
namespace {

using microide::util::JsonValue;
using microide::util::ParseJson;
using microide::util::SerializeJson;
namespace codec = microide::workspace::dap_protocol;

JsonValue Json(std::string_view text) {
  std::optional<JsonValue> parsed = ParseJson(text);
  Expect(parsed.has_value(), "test JSON literal should parse");
  return std::move(*parsed);
}

void TestDapProtocolEncodesRequestEnvelope() {
  const JsonValue request = codec::MakeRequest(7, "setBreakpoints", Json(R"({"source":{"path":"a.c"}})"));
  Expect(request["seq"].AsInt() == 7, "request carries seq");
  Expect(request["type"].AsString() == "request", "request type is request");
  Expect(request["command"].AsString() == "setBreakpoints", "request command set");
  Expect(request["arguments"]["source"]["path"].AsString() == "a.c", "arguments forwarded verbatim");

  // Null arguments are omitted entirely.
  const JsonValue no_args = codec::MakeRequest(8, "threads", JsonValue(nullptr));
  Expect(!no_args.HasKey("arguments"), "null arguments omitted");
}

void TestDapProtocolEncodesResponseEnvelope() {
  const JsonValue ok =
      codec::MakeResponse(20, 7, "setBreakpoints", true, "", Json(R"({"breakpoints":[]})"));
  Expect(ok["seq"].AsInt() == 20 && ok["request_seq"].AsInt() == 7, "response seq + request_seq");
  Expect(ok["type"].AsString() == "response", "response type");
  Expect(ok["success"].AsBool(false), "response success flag");
  Expect(!ok.HasKey("message"), "empty message omitted on success");

  const JsonValue err =
      codec::MakeResponse(21, 9, "evaluate", false, "not available", JsonValue(nullptr));
  Expect(!err["success"].AsBool(true), "error response success is false");
  Expect(err["message"].AsString() == "not available", "error message included");
  Expect(!err.HasKey("body"), "null body omitted");
}

void TestDapProtocolParsesResponse() {
  const codec::DapResponse ok =
      codec::ParseResponse(Json(R"({"type":"response","request_seq":7,"success":true,
          "command":"stackTrace","body":{"totalFrames":3}})"));
  Expect(ok.success, "parsed success");
  Expect(ok.command == "stackTrace", "parsed command");
  Expect(ok.body["totalFrames"].AsInt() == 3, "parsed body");

  const codec::DapResponse err = codec::ParseResponse(
      Json(R"({"type":"response","request_seq":9,"success":false,"message":"boom"})"));
  Expect(!err.success && err.message == "boom", "parsed error message");
}

void TestDapProtocolParsesCapabilities() {
  const codec::DapCapabilities caps = codec::ParseCapabilities(
      Json(R"({"supportsConfigurationDoneRequest":true,"supportsConditionalBreakpoints":true,
          "supportsSetVariable":true,"supportsEvaluateForHovers":true,"supportsLogPoints":true,
          "supportsRestartRequest":true,
          "exceptionBreakpointFilters":[
            {"filter":"raised","label":"Raised Exceptions"},
            {"filter":"uncaught","label":"Uncaught Exceptions","default":true,
             "supportsCondition":true}]})"));
  Expect(caps.supports_configuration_done_request, "configurationDone capability");
  Expect(caps.supports_conditional_breakpoints, "conditional breakpoints capability");
  Expect(caps.supports_set_variable, "setVariable capability");
  Expect(caps.supports_evaluate_for_hovers, "evaluate-for-hovers capability");
  Expect(caps.supports_log_points, "logpoints capability");
  Expect(caps.supports_restart_request, "restart-request capability (Phase 7)");
  // Exception filters parse with ids/labels/defaults (Phase 7).
  Expect(caps.exception_filters.size() == 2, "both advertised exception filters parse");
  Expect(caps.exception_filters[0].filter == "raised" &&
             caps.exception_filters[0].label == "Raised Exceptions" &&
             !caps.exception_filters[0].default_enabled,
         "first filter id/label/default parse");
  Expect(caps.exception_filters[1].default_enabled && caps.exception_filters[1].supports_condition,
         "second filter's default + supportsCondition parse");
  // Absent flags default to false.
  Expect(!caps.supports_step_back, "absent capability defaults to false");
}

void TestDapProtocolParsesStoppedEvent() {
  const codec::DapStoppedEvent stopped = codec::ParseStoppedEvent(
      Json(R"({"reason":"breakpoint","threadId":1,"allThreadsStopped":true,
          "hitBreakpointIds":[4,5]})"));
  Expect(stopped.reason == "breakpoint", "stopped reason");
  Expect(stopped.thread_id == 1, "stopped thread id");
  Expect(stopped.all_threads_stopped, "all threads stopped flag");
  Expect(stopped.hit_breakpoint_ids.size() == 2 && stopped.hit_breakpoint_ids[1] == 5,
         "hit breakpoint ids parsed");
}

void TestDapProtocolParsesOutputEvent() {
  const codec::DapOutputEvent output =
      codec::ParseOutputEvent(Json(R"({"category":"stderr","output":"oops\n"})"));
  Expect(output.category == "stderr", "output category");
  Expect(output.output == "oops\n", "output text");
  // Missing category defaults to console.
  const codec::DapOutputEvent fallback = codec::ParseOutputEvent(Json(R"({"output":"hi"})"));
  Expect(fallback.category == "console", "missing category defaults to console");
}

void TestDapProtocolParsesThreadsAndStackAndScopesAndVariables() {
  const auto threads = codec::ParseThreads(Json(R"({"threads":[{"id":1,"name":"main"}]})"));
  Expect(threads.size() == 1 && threads[0].id == 1 && threads[0].name == "main", "threads parsed");

  const auto frames = codec::ParseStackFrames(
      Json(R"({"stackFrames":[{"id":1000,"name":"f","line":12,"column":3,
          "source":{"name":"a.c","path":"/tmp/a.c"}}]})"));
  Expect(frames.size() == 1, "one frame parsed");
  Expect(frames[0].id == 1000 && frames[0].line == 12, "frame id/line");
  Expect(frames[0].source.path == "/tmp/a.c", "frame source path");

  const auto scopes = codec::ParseScopes(Json(
      R"({"scopes":[{"name":"Locals","variablesReference":42,"expensive":false,"namedVariables":9}]})"));
  Expect(scopes.size() == 1 && scopes[0].variables_reference == 42, "scope ref");
  Expect(scopes[0].named_variables == 9,
         "scope namedVariables parsed (used to clamp the variables fetch count)");

  const auto variables = codec::ParseVariables(
      Json(R"({"variables":[{"name":"x","value":"5","type":"int","variablesReference":0},
          {"name":"p","value":"{...}","variablesReference":99}]})"));
  Expect(variables.size() == 2, "two variables parsed");
  Expect(variables[0].name == "x" && variables[0].value == "5" && variables[0].type == "int",
         "scalar variable parsed");
  Expect(variables[1].variables_reference == 99, "structured variable has child ref");
}

void TestDapProtocolParsesBreakpointsAndEvaluate() {
  const auto breakpoints = codec::ParseBreakpoints(
      Json(R"({"breakpoints":[{"id":1,"verified":true,"line":10},
          {"id":2,"verified":false,"message":"no code","line":11}]})"));
  Expect(breakpoints.size() == 2, "two breakpoints parsed");
  Expect(breakpoints[0].verified && breakpoints[0].line == 10, "verified breakpoint");
  Expect(!breakpoints[1].verified && breakpoints[1].message == "no code", "unverified breakpoint");

  const codec::DapEvaluateResult eval = codec::ParseEvaluateResult(
      Json(R"({"result":"42","type":"int","variablesReference":0})"));
  Expect(eval.result == "42" && eval.type == "int", "evaluate result parsed");
}

void TestDapProtocolEncodesVariablesRequests() {
  const JsonValue stack = codec::MakeStackTraceArguments(3, 0, 0);
  Expect(stack["threadId"].AsInt() == 3 && stack["startFrame"].AsInt() == 0 &&
             stack["levels"].AsInt() == 0,
         "stackTrace arguments carry threadId/startFrame/levels");

  const JsonValue scopes = codec::MakeScopesArguments(11);
  Expect(scopes["frameId"].AsInt() == 11, "scopes arguments carry the frame id");

  // start/count are omitted when zero (whole-set fetch), present otherwise.
  const JsonValue whole = codec::MakeVariablesArguments(42, 0, 0);
  Expect(whole["variablesReference"].AsInt() == 42, "variables arguments carry the reference");
  Expect(!whole.HasKey("start") && !whole.HasKey("count"), "zero paging fields are omitted");
  const JsonValue paged = codec::MakeVariablesArguments(42, 10, 20);
  Expect(paged["start"].AsInt() == 10 && paged["count"].AsInt() == 20, "nonzero paging is sent");

  const JsonValue set = codec::MakeSetVariableArguments(
      codec::SetVariableInput{.variables_reference = 1000, .name = "x", .value = "99"});
  Expect(set["variablesReference"].AsInt() == 1000 && set["name"].AsString() == "x" &&
             set["value"].AsString() == "99",
         "setVariable arguments carry the container ref, name, and value");

  const codec::DapSetVariableResult result = codec::ParseSetVariableResult(
      Json(R"({"value":"99","type":"int","variablesReference":0})"));
  Expect(result.value == "99" && result.type == "int" && result.variables_reference == 0,
         "setVariable result parsed");

  // evaluate (hover-to-inspect): expression + context always present. frameId is
  // sent for any id >= 0 (gdb's top frame is id 0, so frame 0 must be evaluated in
  // that frame, not global scope) and omitted only for the -1 "no frame" sentinel.
  const JsonValue eval = codec::MakeEvaluateArguments("count", 7, "hover");
  Expect(eval["expression"].AsString() == "count" && eval["frameId"].AsInt() == 7 &&
             eval["context"].AsString() == "hover",
         "evaluate arguments carry expression, frameId, and context");
  const JsonValue eval_frame_zero = codec::MakeEvaluateArguments("count", 0, "hover");
  Expect(eval_frame_zero.HasKey("frameId") && eval_frame_zero["frameId"].AsInt() == 0,
         "evaluate sends frameId for frame 0 (gdb top frame is id 0)");
  const JsonValue eval_no_frame = codec::MakeEvaluateArguments("count", -1, "repl");
  Expect(!eval_no_frame.HasKey("frameId"), "evaluate omits frameId for the -1 no-frame sentinel");
  // Watch expressions reuse the same encoder with context "watch" (Phase 6).
  const JsonValue watch = codec::MakeEvaluateArguments("arr[i]", 7, "watch");
  Expect(watch["expression"].AsString() == "arr[i]" && watch["frameId"].AsInt() == 7 &&
             watch["context"].AsString() == "watch",
         "watch evaluate carries expression, frameId, and the watch context");

  // setExceptionBreakpoints (Phase 7): a `filters` array of the enabled ids.
  const JsonValue exc = codec::MakeSetExceptionBreakpointsArguments({"raised", "uncaught"});
  Expect(exc.HasKey("filters") && exc["filters"].IsArray() &&
             exc["filters"].AsArray().size() == 2,
         "setExceptionBreakpoints carries a filters array");
  Expect(exc["filters"].AsArray()[0].AsString() == "raised" &&
             exc["filters"].AsArray()[1].AsString() == "uncaught",
         "the filters array preserves the enabled ids in order");
  const JsonValue exc_empty = codec::MakeSetExceptionBreakpointsArguments({});
  Expect(exc_empty["filters"].IsArray() && exc_empty["filters"].AsArray().empty(),
         "an empty enabled set encodes an empty filters array");
}

void TestDapProtocolEncodesFunctionBreakpoints() {
  std::vector<codec::SetFunctionBreakpointInput> inputs = {
      codec::SetFunctionBreakpointInput{.name = "main"},
      codec::SetFunctionBreakpointInput{.name = "compute", .condition = "n > 0",
                                        .hit_condition = ">3"},
  };
  const JsonValue args = codec::MakeSetFunctionBreakpointsArguments(inputs);
  Expect(args.HasKey("breakpoints") && args["breakpoints"].AsArray().size() == 2,
         "setFunctionBreakpoints carries a breakpoints array");
  const JsonValue& first = args["breakpoints"].AsArray()[0];
  Expect(first["name"].AsString() == "main", "the first breakpoint carries its name");
  Expect(!first.HasKey("condition") && !first.HasKey("hitCondition"),
         "empty condition/hitCondition fields are omitted");
  const JsonValue& second = args["breakpoints"].AsArray()[1];
  Expect(second["name"].AsString() == "compute" && second["condition"].AsString() == "n > 0" &&
             second["hitCondition"].AsString() == ">3",
         "a conditioned function breakpoint carries condition + hitCondition");

  // The response body reuses ParseBreakpoints; gdb reports `reason` not `message`.
  const codec::DapBreakpoint pending =
      codec::ParseBreakpoint(Json(R"({"id":4,"verified":false,"reason":"pending"})"));
  Expect(pending.id == 4 && !pending.verified && pending.message == "pending",
         "an unverified breakpoint surfaces gdb's reason as the message");
}

void TestDapProtocolEncodesExceptionFilterOptions() {
  // Conditioned filters go in filterOptions; unconditioned ones stay in filters.
  const JsonValue args = codec::MakeSetExceptionBreakpointsArguments(
      {"uncaught"}, {{"throw", "x == 2"}, {"catch", ""}});
  Expect(args["filters"].AsArray().size() == 1 &&
             args["filters"].AsArray()[0].AsString() == "uncaught",
         "plain (unconditioned) filters stay in the filters array");
  Expect(args.HasKey("filterOptions") && args["filterOptions"].AsArray().size() == 2,
         "conditioned filters are carried in filterOptions");
  const JsonValue& opt = args["filterOptions"].AsArray()[0];
  Expect(opt["filterId"].AsString() == "throw" && opt["condition"].AsString() == "x == 2",
         "a filter option carries its filterId + condition");
  const JsonValue& opt_empty = args["filterOptions"].AsArray()[1];
  Expect(opt_empty["filterId"].AsString() == "catch" && !opt_empty.HasKey("condition"),
         "an empty condition is omitted from the filter option");
  // The capabilities parser reads supportsExceptionFilterOptions + per-filter
  // supportsCondition (both gdb 17.2 advertises).
  const codec::DapCapabilities caps = codec::ParseCapabilities(Json(
      R"({"supportsExceptionFilterOptions":true,"exceptionBreakpointFilters":[{"filter":"throw","label":"Throw","supportsCondition":true}]})"));
  Expect(caps.supports_exception_filter_options, "supportsExceptionFilterOptions parses");
  Expect(caps.exception_filters.size() == 1 && caps.exception_filters[0].supports_condition,
         "a filter's supportsCondition parses");
}

}  // namespace

void RegisterDapProtocolTests(std::vector<TestCase>& tests) {
  AddTest(tests, "DapProtocol/EncodesRequestEnvelope", TestDapProtocolEncodesRequestEnvelope);
  AddTest(tests, "DapProtocol/EncodesResponseEnvelope", TestDapProtocolEncodesResponseEnvelope);
  AddTest(tests, "DapProtocol/ParsesResponse", TestDapProtocolParsesResponse);
  AddTest(tests, "DapProtocol/ParsesCapabilities", TestDapProtocolParsesCapabilities);
  AddTest(tests, "DapProtocol/ParsesStoppedEvent", TestDapProtocolParsesStoppedEvent);
  AddTest(tests, "DapProtocol/ParsesOutputEvent", TestDapProtocolParsesOutputEvent);
  AddTest(tests, "DapProtocol/ParsesThreadsStackScopesVariables",
          TestDapProtocolParsesThreadsAndStackAndScopesAndVariables);
  AddTest(tests, "DapProtocol/ParsesBreakpointsAndEvaluate",
          TestDapProtocolParsesBreakpointsAndEvaluate);
  AddTest(tests, "DapProtocol/EncodesVariablesRequests", TestDapProtocolEncodesVariablesRequests);
  AddTest(tests, "DapProtocol/EncodesFunctionBreakpoints",
          TestDapProtocolEncodesFunctionBreakpoints);
  AddTest(tests, "DapProtocol/EncodesExceptionFilterOptions",
          TestDapProtocolEncodesExceptionFilterOptions);
}

}  // namespace microide::tests
