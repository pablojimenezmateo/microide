#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace microide::workspace {

// Parsed `control-send` invocation. Exposed so the argument grammar and request
// shaping can be unit-tested without a socket.
struct ControlSendOptions {
  // Target selection (at most one). When neither is set, discovery auto-selects
  // the sole running instance.
  std::optional<int> pid;
  std::optional<std::filesystem::path> socket;

  // Request shape — exactly one of these must be provided.
  std::vector<std::string> command_tokens;     // raw command line (joined)
  std::optional<std::string> query_verb;       // --query <verb>
  std::optional<std::string> query_args_json;  // --args <json> (with --query)
  std::optional<std::string> raw_json;         // --json <object> (sent verbatim)

  int timeout_seconds = 5;
  std::optional<std::string> wait_event;  // --wait <event>: block until it arrives
  int request_id = 1;

  bool valid = false;
  std::string error;
};

// Parse the tokens that follow `control-send` (i.e. argv[2..]).
ControlSendOptions ParseControlSendArgs(const std::vector<std::string>& args);

// Build the JSON request line from parsed options. Returns nullopt and sets
// *error when no shape is given or a JSON payload is malformed.
std::optional<std::string> BuildControlSendRequest(const ControlSendOptions& options,
                                                   std::string* error);

// Entry point for `microide control-send`. Connects to the target instance,
// sends one request, prints every received JSONL line to stdout, and returns the
// process exit code: 0 (reply ok), 1 (reply not ok), 2 (usage/discovery/connect/
// timeout), 3 (--wait event never arrived).
int RunControlSend(int argc, char** argv);

}  // namespace microide::workspace
