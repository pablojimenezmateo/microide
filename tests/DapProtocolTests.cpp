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
          "supportsSetVariable":true,"supportsEvaluateForHovers":true,"supportsLogPoints":true})"));
  Expect(caps.supports_configuration_done_request, "configurationDone capability");
  Expect(caps.supports_conditional_breakpoints, "conditional breakpoints capability");
  Expect(caps.supports_set_variable, "setVariable capability");
  Expect(caps.supports_evaluate_for_hovers, "evaluate-for-hovers capability");
  Expect(caps.supports_log_points, "logpoints capability");
  // Absent flags default to false.
  Expect(!caps.supports_restart_request, "absent capability defaults to false");
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

  const auto scopes = codec::ParseScopes(
      Json(R"({"scopes":[{"name":"Locals","variablesReference":42,"expensive":false}]})"));
  Expect(scopes.size() == 1 && scopes[0].variables_reference == 42, "scope ref");

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

  // evaluate (hover-to-inspect): expression + context always present; frameId omitted when 0.
  const JsonValue eval = codec::MakeEvaluateArguments("count", 7, "hover");
  Expect(eval["expression"].AsString() == "count" && eval["frameId"].AsInt() == 7 &&
             eval["context"].AsString() == "hover",
         "evaluate arguments carry expression, frameId, and context");
  const JsonValue eval_no_frame = codec::MakeEvaluateArguments("count", 0, "hover");
  Expect(!eval_no_frame.HasKey("frameId"), "evaluate omits frameId when zero");
  // Watch expressions reuse the same encoder with context "watch" (Phase 6).
  const JsonValue watch = codec::MakeEvaluateArguments("arr[i]", 7, "watch");
  Expect(watch["expression"].AsString() == "arr[i]" && watch["frameId"].AsInt() == 7 &&
             watch["context"].AsString() == "watch",
         "watch evaluate carries expression, frameId, and the watch context");
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
}

}  // namespace microide::tests
