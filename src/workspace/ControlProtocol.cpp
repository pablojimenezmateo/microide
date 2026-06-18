#include "workspace/ControlProtocol.h"

#include <array>

namespace microide::workspace {

namespace {

constexpr std::array<std::string_view, 5> kQueryVerbs = {
    "debug-state", "breakpoints", "tabs", "projects", "status",
};

}  // namespace

std::span<const std::string_view> ControlQueryVerbs() {
  return {kQueryVerbs.data(), kQueryVerbs.size()};
}

ControlRequest ParseControlRequest(std::string_view json_line) {
  ControlRequest request;
  std::optional<util::JsonValue> parsed = util::ParseJson(json_line);
  if (!parsed) {
    request.parse_error = "invalid JSON";
    return request;
  }
  if (!parsed->IsObject()) {
    request.parse_error = "request must be a JSON object";
    return request;
  }

  if (parsed->HasKey("id")) {
    const util::JsonValue& id = (*parsed)["id"];
    if (id.IsInt()) {
      request.id = id.AsInt();
    } else if (!id.IsNull()) {
      request.parse_error = "\"id\" must be an integer";
      return request;
    }
  }

  const bool has_command = parsed->HasKey("command") && (*parsed)["command"].IsString();
  const bool has_query = parsed->HasKey("query") && (*parsed)["query"].IsString();
  if (has_command == has_query) {
    request.parse_error = "request must have exactly one of \"command\" or \"query\"";
    return request;
  }

  if (has_command) {
    request.command = (*parsed)["command"].AsString();
    if (request.command.empty()) {
      request.parse_error = "\"command\" must be non-empty";
      return request;
    }
  } else {
    request.query = (*parsed)["query"].AsString();
    if (request.query.empty()) {
      request.parse_error = "\"query\" must be non-empty";
      return request;
    }
    if (parsed->HasKey("args")) {
      request.args = (*parsed)["args"];
    }
  }

  request.valid = true;
  return request;
}

std::string SerializeControlResponse(const ControlResponse& response) {
  util::JsonObject object;
  if (response.id) {
    object["id"] = util::JsonValue(*response.id);
  }
  object["ok"] = util::JsonValue(response.ok);
  if (!response.feedback.empty()) {
    object["feedback"] = util::JsonValue(response.feedback);
  }
  if (!response.error.empty()) {
    object["error"] = util::JsonValue(response.error);
  }
  if (response.result) {
    object["result"] = *response.result;
  }
  return util::SerializeJson(util::JsonValue(std::move(object)));
}

std::string SerializeControlEvent(const util::JsonValue& event) {
  return util::SerializeJson(event);
}

std::string ControlChannelHelpText() {
  std::string text;
  text +=
      "microide control channel\n"
      "========================\n"
      "\n"
      "An external tool (e.g. an LLM) can drive a running microide instance: set\n"
      "breakpoints, step through a debug session, open/close files, switch projects,\n"
      "open terminals, and observe where execution stopped.\n"
      "\n"
      "Enabling\n"
      "--------\n"
      "The channel is OFF by default. Turn on the `control.enabled` setting (Settings\n"
      "overlay, Ctrl+, or the `settings` command). It starts immediately on the running\n"
      "instance -- no restart needed.\n"
      "\n"
      "Discovery\n"
      "---------\n"
      "Each running instance with the channel on listens on a private Unix domain\n"
      "socket (mode 0600) under $XDG_RUNTIME_DIR/microide/ and writes a descriptor\n"
      "file to $XDG_RUNTIME_DIR/microide/instances/<pid>.json:\n"
      "  {\"pid\":..,\"socket\":\"..\",\"project_root\":\"..\",\"project_hash\":\"..\"}\n"
      "Run `microide control-list` to print descriptors for all running instances.\n"
      "\n"
      "Protocol (newline-delimited JSON; one object per line)\n"
      "------------------------------------------------------\n"
      "Request (run a command):\n"
      "  {\"id\":1,\"command\":\"breakpoint-set src/main.cpp 42\"}\n"
      "Request (read state):\n"
      "  {\"id\":2,\"query\":\"debug-state\"}\n"
      "Response (command):\n"
      "  {\"id\":1,\"ok\":true,\"feedback\":\"...\"}   or   {\"id\":1,\"ok\":false,\"error\":\"...\"}\n"
      "Response (query):\n"
      "  {\"id\":2,\"ok\":true,\"result\":{...}}\n"
      "Unsolicited events (pushed to every connected client):\n"
      "  {\"event\":\"stopped\",\"file\":\"main.cpp\",\"line\":42,\"reason\":\"breakpoint\",\"frames\":[...]}\n"
      "  {\"event\":\"terminated\",\"sessionId\":..}\n"
      "  {\"event\":\"output\",\"category\":\"stdout\",\"text\":\"..\"}\n"
      "Lines are 1-based for the developer-facing surface (commands, spec, events);\n"
      "they map to microide's internal 0-based buffer lines automatically.\n"
      "\n"
      "Query verbs\n"
      "-----------\n";
  for (std::string_view verb : ControlQueryVerbs()) {
    text += "  ";
    text += verb;
    text += '\n';
  }
  text +=
      "\n"
      "Commands\n"
      "--------\n"
      "Any command from the command palette works. Run `microide control-commands` for\n"
      "the full list. Breakpoint commands added for headless control:\n"
      "  breakpoint-set <file> <line> [condition]\n"
      "  breakpoint-remove <file> <line>\n"
      "  breakpoint-enable <file> <line>\n"
      "  breakpoint-disable <file> <line>\n"
      "  breakpoint-condition <file> <line> [expr]      (omit expr to clear)\n"
      "  breakpoint-hit-condition <file> <line> [expr]  (omit expr to clear)\n"
      "  breakpoint-logmessage <file> <line> [message]  (omit message to clear)\n"
      "  breakpoint-clear [file]                         (omit file to clear all)\n"
      "Debug control: debug-start, debug-continue, debug-step-over, debug-step-in,\n"
      "  debug-step-out, debug-pause, debug-restart, debug-stop. (Breakpoint and debug\n"
      "  commands require the `debug.enabled` setting.)\n"
      "\n"
      "Cold-start spec (microide --control-spec <file.json>)\n"
      "-----------------------------------------------------\n"
      "Open a project with breakpoints already set before the window is interactive:\n"
      "  {\n"
      "    \"project\": \"/path/to/project\",          // optional; selects the project\n"
      "    \"breakpoints\": [\n"
      "      {\"file\":\"src/main.cpp\",\"line\":42},\n"
      "      {\"file\":\"src/util.cpp\",\"line\":120,\"condition\":\"x>10\"},\n"
      "      {\"file\":\"src/log.cpp\",\"line\":7,\"logMessage\":\"hit {x}\"},\n"
      "      {\"file\":\"src/a.cpp\",\"line\":3,\"hitCondition\":\">=5\",\"enabled\":false}\n"
      "    ],\n"
      "    \"open\": [\"src/main.cpp\"],               // optional files to reveal\n"
      "    \"launch\": \"Python: main\",                // optional launch-config to auto-start\n"
      "    \"commands\": [\"sidebar-hide\"]            // optional raw command lines, run last\n"
      "  }\n"
      "Spec line numbers are 1-based; relative file paths resolve against the project\n"
      "root.\n";
  return text;
}

}  // namespace microide::workspace
