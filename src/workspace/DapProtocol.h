#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "util/JsonValue.h"

// Single home for the JSON <-> Debug Adapter Protocol wire-type mapping. Every
// place that builds or parses DAP request/response/event payloads routes through
// these helpers so the encoding lives in exactly one translation unit (mirrors
// workspace/LspProtocol.h for the LSP client).
//
// DAP framing reuses the LSP `Content-Length` header; the difference is the
// message envelope: messages carry a monotonic `seq`, a `type`
// ("request"/"response"/"event"), and responses reference the originating
// request via `request_seq`.
namespace microide::workspace::dap_protocol {

// ---- Adapter capabilities (subset we act on) ------------------------------
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
};

// ---- Response / event envelopes -------------------------------------------
struct DapResponse {
  bool success = false;
  std::string command;
  std::string message;   // populated when !success
  util::JsonValue body;  // result payload (object/array/Null)
};

// ---- Body payload structs --------------------------------------------------
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
};

struct DapVariable {
  std::string name;
  std::string value;
  std::string type;
  std::string evaluate_name;
  int variables_reference = 0;  // > 0 means structured/expandable
  int named_variables = 0;
  int indexed_variables = 0;
};

struct DapBreakpoint {
  int id = 0;
  bool verified = false;
  std::string message;
  int line = 0;
  int column = 0;
};

struct DapStoppedEvent {
  std::string reason;       // breakpoint, step, pause, exception, ...
  std::string description;
  std::string text;
  int thread_id = 0;
  bool all_threads_stopped = false;
  std::vector<int> hit_breakpoint_ids;
};

struct DapOutputEvent {
  std::string category;  // stdout, stderr, console, telemetry, important
  std::string output;
  int variables_reference = 0;
};

struct DapEvaluateResult {
  std::string result;
  std::string type;
  int variables_reference = 0;
};

// ---- Encode (structs -> wire JSON) ----------------------------------------
util::JsonValue MakeRequest(int seq, const std::string& command,
                            const util::JsonValue& arguments);
util::JsonValue MakeResponse(int seq, int request_seq, const std::string& command, bool success,
                             const std::string& message, util::JsonValue body);

// ---- Decode (wire JSON -> structs) ----------------------------------------
// `msg` is the full response envelope.
DapResponse ParseResponse(const util::JsonValue& msg);
// `body` is the `capabilities` body of an initialize response (or any object
// carrying the supports* flags, e.g. the `capabilities` event body).
DapCapabilities ParseCapabilities(const util::JsonValue& body);

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
DapOutputEvent ParseOutputEvent(const util::JsonValue& body);
DapEvaluateResult ParseEvaluateResult(const util::JsonValue& body);

}  // namespace microide::workspace::dap_protocol
