#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "util/JsonValue.h"

namespace microide::workspace {

// Canonical query verbs understood by the control channel. Shared by the
// dispatcher, the help text, and the drift-guard test so the three never
// disagree.
std::span<const std::string_view> ControlQueryVerbs();

// One decoded request from the control channel. Exactly one of `command` /
// `query` is set on a valid request; `command` runs through the same
// CommandPromptCoordinator::ExecuteCommandLine chokepoint as the in-app command
// palette, `query` reads workspace state and returns structured JSON. The optional `id` is
// echoed back on the matching response so a client can correlate concurrent
// requests.
struct ControlRequest {
  bool valid = false;
  std::string parse_error;  // populated when !valid
  std::optional<std::int64_t> id;
  std::string command;  // non-empty for a command request
  std::string query;    // non-empty for a query request
  util::JsonValue args;  // optional structured args (queries)

  bool is_command() const { return !command.empty(); }
  bool is_query() const { return !query.empty(); }
};

// Decode a single JSONL line into a ControlRequest. Never throws; malformed
// input yields {valid=false, parse_error=...}.
ControlRequest ParseControlRequest(std::string_view json_line);

// Reply to one request. `result` is carried only for query responses.
struct ControlResponse {
  std::optional<std::int64_t> id;
  bool ok = false;
  std::string feedback;
  std::string error;
  std::optional<util::JsonValue> result;
};

// Serialize to a single compact JSON line (no trailing newline; the socket
// layer frames with '\n').
std::string SerializeControlResponse(const ControlResponse& response);

// Async, unsolicited notifications pushed to every connected client. Built as
// JsonValue objects so callers can attach event-specific bodies; serialized the
// same way as responses.
std::string SerializeControlEvent(const util::JsonValue& event);

// Self-documentation rendered by `microide control-help`, the man page, and the
// dev doc. Single source of truth for the human-readable protocol description so
// the shipped docs cannot drift from the implementation.
std::string ControlChannelHelpText();

}  // namespace microide::workspace
