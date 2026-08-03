#include "TestSupport.h"

#include "util/JsonValue.h"
#include "workspace/debug/DapProtocol.h"
#include "workspace/JsonRpcMessageFraming.h"

#include <cstdint>
#include <optional>
#include <string>

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
          "supportsRestartRequest":true,"supportsStepBack":true,
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
  Expect(caps.supports_step_back, "step-back capability (reverse execution)");
  // Exception filters parse with ids/labels/defaults (Phase 7).
  Expect(caps.exception_filters.size() == 2, "both advertised exception filters parse");
  Expect(caps.exception_filters[0].filter == "raised" &&
             caps.exception_filters[0].label == "Raised Exceptions" &&
             !caps.exception_filters[0].default_enabled,
         "first filter id/label/default parse");
  Expect(caps.exception_filters[1].default_enabled && caps.exception_filters[1].supports_condition,
         "second filter's default + supportsCondition parse");
  // Absent flags default to false.
  Expect(!caps.supports_function_breakpoints, "absent capability defaults to false");
}

void TestDapProtocolMergesPartialCapabilities() {
  // The DAP `capabilities` event carries a *partial* Capabilities object. A merge
  // must flip only the stated fields and preserve everything initialize advertised —
  // re-running ParseCapabilities would reset the absent flags to false. This is the
  // gdb-under-rr/record case: it turns supportsStepBack on after initialize.
  codec::DapCapabilities caps = codec::ParseCapabilities(
      Json(R"({"supportsConfigurationDoneRequest":true,"supportsSetVariable":true,
          "exceptionBreakpointFilters":[{"filter":"raised","label":"Raised"}]})"));
  Expect(!caps.supports_step_back, "step-back starts off (gdb omits it at init)");

  codec::MergeCapabilities(caps, Json(R"({"supportsStepBack":true})"));
  Expect(caps.supports_step_back, "partial merge turns step-back on");
  Expect(caps.supports_configuration_done_request,
         "partial merge preserves an unrelated capability");
  Expect(caps.supports_set_variable, "partial merge preserves setVariable");
  Expect(caps.exception_filters.size() == 1,
         "partial merge preserves exception filters it does not restate");

  // An explicit false in the body turns a flag back off.
  codec::MergeCapabilities(caps, Json(R"({"supportsStepBack":false})"));
  Expect(!caps.supports_step_back, "explicit false in the body turns step-back off");
  // Restating the filter array replaces it wholesale.
  codec::MergeCapabilities(
      caps, Json(R"({"exceptionBreakpointFilters":[{"filter":"a","label":"A"},
          {"filter":"b","label":"B"}]})"));
  Expect(caps.exception_filters.size() == 2, "restated exception-filter array replaces wholesale");
}

void TestDapProtocolParsesStoppedEvent() {
  const codec::DapStoppedEvent stopped = codec::ParseStoppedEvent(
      Json(R"({"reason":"breakpoint","threadId":1,"allThreadsStopped":true,
          "hitBreakpointIds":[4,5]})"));
  Expect(stopped.reason == "breakpoint", "stopped reason");
  Expect(stopped.thread_id.has_value() && *stopped.thread_id == 1, "stopped thread id");
  Expect(stopped.all_threads_stopped, "all threads stopped flag");
  Expect(stopped.hit_breakpoint_ids.size() == 2 && stopped.hit_breakpoint_ids[1] == 5,
         "hit breakpoint ids parsed");

  // J9: threadId is optional on `stopped`. When the adapter omits it (e.g. an
  // all-threads-stopped halt) the id must be UNSET, never defaulted to 0 — the
  // session resolves a real thread rather than requesting threadId:0.
  const codec::DapStoppedEvent no_thread =
      codec::ParseStoppedEvent(Json(R"({"reason":"pause","allThreadsStopped":true})"));
  Expect(!no_thread.thread_id.has_value(),
         "a stopped event without threadId leaves the thread id unset (not 0)");
  Expect(no_thread.all_threads_stopped, "allThreadsStopped still parses without a threadId");
}

// E4: a `continued` event distinguishes a full resume (allThreadsContinued=true)
// from a single-thread continue, and keeps threadId optional (unset, not 0).
void TestDapProtocolParsesContinuedEvent() {
  const codec::DapContinuedEvent full = codec::ParseContinuedEvent(
      Json(R"({"threadId":3,"allThreadsContinued":true})"));
  Expect(full.thread_id.has_value() && *full.thread_id == 3, "continued thread id");
  Expect(full.all_threads_continued, "allThreadsContinued=true is a full resume");

  const codec::DapContinuedEvent partial =
      codec::ParseContinuedEvent(Json(R"({"threadId":2,"allThreadsContinued":false})"));
  Expect(partial.thread_id.has_value() && *partial.thread_id == 2, "partial continued thread id");
  Expect(!partial.all_threads_continued, "allThreadsContinued=false is a single-thread continue");

  // Missing allThreadsContinued is treated as NOT-all (conservative).
  const codec::DapContinuedEvent bare = codec::ParseContinuedEvent(Json(R"({"threadId":5})"));
  Expect(bare.thread_id.has_value() && *bare.thread_id == 5, "bare continued thread id");
  Expect(!bare.all_threads_continued, "absent allThreadsContinued defaults to single-thread");

  const codec::DapContinuedEvent empty = codec::ParseContinuedEvent(Json("null"));
  Expect(!empty.thread_id.has_value() && !empty.all_threads_continued,
         "continued from a null body → unset thread, not-all");
}

// J26: protocol numeric fields narrow to `int` deterministically. An out-of-int-
// range id/position must clamp to the nearest int bound, never wrap to an
// unrelated small/negative value.
void TestDapProtocolClampsOutOfRangeInts() {
  constexpr std::int64_t kIntMax = 2147483647;
  constexpr std::int64_t kIntMin = -2147483647 - 1;

  // INT_MAX + 1 and INT64_MAX both clamp to INT_MAX (stay large + positive).
  const codec::DapStackFrame over = codec::ParseStackFrame(
      Json(R"({"id":2147483648,"line":9223372036854775807,"column":1})"));
  Expect(over.id == static_cast<int>(kIntMax),
         "a frame id of INT_MAX+1 clamps to INT_MAX (no wrap to negative)");
  Expect(over.line == static_cast<int>(kIntMax), "a line of INT64_MAX clamps to INT_MAX");

  // A huge NEGATIVE id clamps to INT_MIN (stays negative, does not wrap positive).
  const codec::DapStackFrame under =
      codec::ParseStackFrame(Json(R"({"id":-9223372036854775808,"line":1,"column":1})"));
  Expect(under.id == static_cast<int>(kIntMin), "a huge negative id clamps to INT_MIN");

  // An in-range value round-trips unchanged.
  const codec::DapThread thread = codec::ParseThread(Json(R"({"id":123456,"name":"t"})"));
  Expect(thread.id == 123456, "an in-range id is preserved");
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

// Malformed / truncated / wrong-typed wire input must never crash the decoders:
// a real adapter (or a corrupt stream) can send anything. The decoders are
// defensive by contract (missing keys → defaults, wrong-typed arrays → empty),
// and this pins that contract so a future "optimization" cannot reintroduce an
// unchecked AsArray()/AsString() that segfaults on a hostile payload.
void TestDapProtocolDecodeRobustness() {
  // Missing payload arrays decode to empty, not a crash.
  Expect(codec::ParseVariables(Json("{}")).empty(), "variables: missing array → empty");
  Expect(codec::ParseScopes(Json("{}")).empty(), "scopes: missing array → empty");
  Expect(codec::ParseStackFrames(Json("{}")).empty(), "stackFrames: missing array → empty");
  Expect(codec::ParseThreads(Json("{}")).empty(), "threads: missing array → empty");
  Expect(codec::ParseBreakpoints(Json("{}")).empty(), "breakpoints: missing array → empty");

  // Wrong-typed payload fields (array key holding a scalar, or items of the wrong
  // shape) degrade to empty / default rather than throwing.
  Expect(codec::ParseVariables(Json(R"({"variables":42})")).empty(),
         "variables: non-array → empty");
  Expect(codec::ParseStackFrames(Json(R"({"stackFrames":"nope"})")).empty(),
         "stackFrames: non-array → empty");
  Expect(codec::ParseThreads(Json(R"({"threads":{}})")).empty(),
         "threads: object-not-array → empty");

  // Items with wrong-typed scalar fields fall back to defaults (no throw): a
  // variablesReference sent as a string, a name sent as a number.
  const auto vars = codec::ParseVariables(
      Json(R"({"variables":[{"name":7,"value":null,"variablesReference":"x"},{}]})"));
  Expect(vars.size() == 2, "two malformed variable items still parse positionally");
  Expect(vars[0].variables_reference == 0, "string variablesReference → 0 fallback");
  Expect(vars[0].name.empty() && vars[1].name.empty(), "non-string / missing name → empty");

  // Response / event envelopes parsed from non-object roots (a bare array, a
  // string, null) must not crash and must report failure / empty fields.
  const codec::DapResponse arr_response = codec::ParseResponse(Json("[1,2,3]"));
  Expect(!arr_response.success && arr_response.command.empty(),
         "response parsed from a bare array → failure, empty command");
  const codec::DapStoppedEvent null_stop = codec::ParseStoppedEvent(Json("null"));
  Expect(null_stop.reason.empty() && !null_stop.thread_id.has_value(),
         "stopped event from null body → empty/unset (threadId absent, not 0)");
  const codec::DapCapabilities str_caps = codec::ParseCapabilities(Json(R"("not-an-object")"));
  Expect(!str_caps.supports_configuration_done_request && str_caps.exception_filters.empty(),
         "capabilities from a string → all-false, no filters");

  // A deeply nested / oversized value (200-level array) decodes without
  // recursion blowups in the parser path that touches it.
  std::string deep = "{\"variables\":[";
  for (int i = 0; i < 200; ++i) {
    if (i != 0) deep += ',';
    deep += R"({"name":"v","value":"0","variablesReference":0})";
  }
  deep += "]}";
  Expect(codec::ParseVariables(Json(deep)).size() == 200, "200-item variables body decodes fully");
}

// A hostile/buggy adapter can ignore the `levels` request cap and return an
// enormous stackFrames array; each frame is materialized into a filesystem::path
// + display strings on the main thread. ParseStackFrames must cap the count so a
// giant array can't stall the UI thread or spike the heap.
void TestDapProtocolStackFramesAreCapped() {
  std::string body = "{\"stackFrames\":[";
  constexpr int kFrames = 50000;
  for (int i = 0; i < kFrames; ++i) {
    if (i != 0) body += ',';
    body += R"({"id":0,"name":"f","line":1,"column":1})";
  }
  body += "]}";
  const auto frames = codec::ParseStackFrames(Json(body));
  Expect(frames.size() <= 10000,
         "stackFrames must be capped, not materialized 1:1 from a hostile array");
}

void TestDapProtocolListsAreCapped() {
  // A hostile/buggy adapter can pack millions of tiny objects into a sub-64 MiB
  // frame; each list parser must clamp like ParseStackFrames rather than
  // materialize the array 1:1 on the main thread.
  constexpr int kEntries = 50000;
  const auto build = [](std::string_view key, std::string_view entry) {
    std::string body = "{\"";
    body += key;
    body += "\":[";
    for (int i = 0; i < kEntries; ++i) {
      if (i != 0) body += ',';
      body += entry;
    }
    body += "]}";
    return body;
  };

  const auto variables =
      codec::ParseVariables(Json(build("variables", R"({"name":"a","value":"1"})")));
  Expect(variables.size() <= 10000, "variables must be capped");

  const auto scopes =
      codec::ParseScopes(Json(build("scopes", R"({"name":"s","variablesReference":1})")));
  Expect(scopes.size() <= 10000, "scopes must be capped");

  const auto threads = codec::ParseThreads(Json(build("threads", R"({"id":1,"name":"t"})")));
  Expect(threads.size() <= 10000, "threads must be capped");

  const auto breakpoints =
      codec::ParseBreakpoints(Json(build("breakpoints", R"({"id":1,"verified":true,"line":1})")));
  Expect(breakpoints.size() <= 10000, "breakpoints must be capped");

  const auto caps =
      codec::ParseCapabilities(Json(build("exceptionBreakpointFilters",
                                          R"({"filter":"f","label":"Filter"})")));
  Expect(caps.exception_filters.size() <= 10000, "exception filters must be capped");

  const auto stopped = codec::ParseStoppedEvent(Json(build("hitBreakpointIds", "1")));
  Expect(stopped.hit_breakpoint_ids.size() <= 10000, "hit breakpoint ids must be capped");
}

}  // namespace

// The DAP client used to carry its OWN copy of the Content-Length framing codec,
// and that copy required a byte-exact `Content-Length: ` prefix. An adapter that
// wrote `content-length:` or omitted the space had its header line dropped as
// garbage, after which the parser tried to resync by reading the JSON body as
// headers — desyncing the stream and tearing the session down. The LSP copy had
// already been hardened for exactly this; the DAP copy never got the fix.
//
// Both now share one framer. This pins the tolerance on the DAP side (with the
// DAP frame ceiling) so the two cannot drift apart again.
void TestDapFramingToleratesHeaderCasingAndSpacing() {
  const std::string body = R"({"type":"event","event":"initialized"})";
  const auto expect_frames = [&](const std::string& header) {
    microide::workspace::JsonRpcMessageFramer framer{
        .max_message_bytes = microide::workspace::kDefaultMaxJsonRpcMessageBytes};
    framer.Append(header + std::to_string(body.size()) + "\r\n\r\n" + body);
    const auto msg = framer.Next();
    Expect(msg.has_value() && (*msg)["event"].AsString() == "initialized",
           "DAP header variant should frame the message: " + header);
  };
  expect_frames("Content-Length: ");    // the only spelling the old DAP copy took
  expect_frames("content-length: ");    // lowercase name
  expect_frames("CONTENT-LENGTH: ");    // uppercase name
  expect_frames("Content-Length:");     // no space after the colon
  expect_frames("Content-Length:   ");  // extra spaces
  expect_frames("Content-Length \t: ");  // whitespace before the colon
}

// A frame past the ceiling must be skipped WHOLE (headers + body) so the parser
// resyncs on the next frame rather than reading body bytes as headers. Pinned on
// the DAP side because the ceiling is now a per-client field rather than a
// hard-coded constant — a wrong or unset one silently swallows every message.
void TestDapFramingOversizedFrameSkipsAndResyncs() {
  microide::workspace::JsonRpcMessageFramer framer{.max_message_bytes = 64};
  const std::string big_body(200, 'x');
  const std::string good_body = R"({"type":"event","event":"terminated"})";
  framer.Append("Content-Length: " + std::to_string(big_body.size()) + "\r\n\r\n" + big_body +
                "Content-Length: " + std::to_string(good_body.size()) + "\r\n\r\n" + good_body);
  // Drain until the framer yields the message behind the oversized frame.
  std::optional<JsonValue> seen;
  for (int i = 0; i < 8 && !seen.has_value(); ++i) {
    seen = framer.Next();
  }
  Expect(seen.has_value() && (*seen)["event"].AsString() == "terminated",
         "the frame after an oversized one must still parse (no desync)");
}

void RegisterDapProtocolTests(std::vector<TestCase>& tests) {
  AddTest(tests, "DapProtocol/FramingToleratesHeaderCasingAndSpacing",
          TestDapFramingToleratesHeaderCasingAndSpacing);
  AddTest(tests, "DapProtocol/FramingOversizedFrameSkipsAndResyncs",
          TestDapFramingOversizedFrameSkipsAndResyncs);
  AddTest(tests, "DapProtocol/DecodeRobustness", TestDapProtocolDecodeRobustness);
  AddTest(tests, "DapProtocol/StackFramesAreCapped", TestDapProtocolStackFramesAreCapped);
  AddTest(tests, "DapProtocol/ListsAreCapped", TestDapProtocolListsAreCapped);
  AddTest(tests, "DapProtocol/EncodesRequestEnvelope", TestDapProtocolEncodesRequestEnvelope);
  AddTest(tests, "DapProtocol/EncodesResponseEnvelope", TestDapProtocolEncodesResponseEnvelope);
  AddTest(tests, "DapProtocol/ParsesResponse", TestDapProtocolParsesResponse);
  AddTest(tests, "DapProtocol/ParsesCapabilities", TestDapProtocolParsesCapabilities);
  AddTest(tests, "DapProtocol/MergesPartialCapabilities", TestDapProtocolMergesPartialCapabilities);
  AddTest(tests, "DapProtocol/ParsesStoppedEvent", TestDapProtocolParsesStoppedEvent);
  AddTest(tests, "DapProtocol/ParsesContinuedEvent", TestDapProtocolParsesContinuedEvent);
  AddTest(tests, "DapProtocol/ClampsOutOfRangeInts", TestDapProtocolClampsOutOfRangeInts);
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
