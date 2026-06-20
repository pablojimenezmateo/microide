#include "workspace/ControlProtocol.h"

#include <array>

namespace microide::workspace {

namespace {

constexpr std::array<std::string_view, 9> kQueryVerbs = {
    "debug-state",          "breakpoints", "function-breakpoints", "exception-filters",
    "tabs",                 "projects",    "status",               "launch-configs",
    "adapters",
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
      "open terminals, and observe where execution stopped. The one-shot CLI\n"
      "`microide control-send` is the easiest way in -- no socket plumbing required.\n"
      "\n"
      "Debug a program in 4 steps (copy-paste)\n"
      "---------------------------------------\n"
      "  microide /path/to/project --set control.enabled true &   # window + live socket\n"
      "  microide control-send --query adapters                   # what can debug (gdb is bundled)\n"
      "  microide control-send breakpoint-function-add main       # debugger auto-enables\n"
      "  microide control-send debug-run ./build/app --wait stopped  # launch; block until it stops\n"
      "control-send auto-discovers the sole running instance, sends one request, prints\n"
      "every JSONL reply/event to stdout, and exits (0 reply-ok, 1 reply-not-ok, 2\n"
      "usage/connect, 3 --wait timed out). `debug-run <program> [args...]` needs no\n"
      "pre-defined launch config; `--wait stopped` returns once execution halts. Leave\n"
      "the window open to hand control to a human.\n"
      "\n"
      "Enabling\n"
      "--------\n"
      "The channel is OFF by default. Three ways to turn it on:\n"
      "  microide --control                 force-start + mirror JSONL to stdout (headless)\n"
      "  microide --set control.enabled true start the socket (transient, not persisted)\n"
      "  Settings overlay (Ctrl+,)          flip `control.enabled` (persisted)\n"
      "It starts immediately on the running instance -- no restart needed.\n"
      "\n"
      "Headless agent stream (microide --control [--control-spec <file>])\n"
      "-----------------------------------------------------------------\n"
      "--control force-starts the channel AND mirrors every response/event to stdout as\n"
      "JSONL, so an agent can drive the instance with no socket client. The stream order:\n"
      "  {\"event\":\"ready\",\"pid\":..,\"socket\":\"..\",\"project_root\":\"..\"}  (handshake, first)\n"
      "  {\"applied\":\"<command>\",\"ok\":true|false,\"error\":\"..\"}        (one per spec entry)\n"
      "  {\"event\":\"output\"|\"stopped\"|\"terminated\",...}              (as the session runs)\n"
      "--set <id> <value> applies a transient (never-persisted) setting override and may\n"
      "be repeated; an explicit project (positional path or spec `project`) wins over a\n"
      "restored session.\n"
      "\n"
      "Discovery\n"
      "---------\n"
      "Each running instance with the channel on listens on a private Unix domain\n"
      "socket (mode 0600) under $XDG_RUNTIME_DIR/microide/ and writes a descriptor\n"
      "file to $XDG_RUNTIME_DIR/microide/instances/<pid>.json:\n"
      "  {\"pid\":..,\"socket\":\"..\",\"project_root\":\"..\",\"project_hash\":\"..\"}\n"
      "Run `microide control-list` to print descriptors for all running instances.\n"
      "\n"
      "One-shot client (microide control-send)\n"
      "---------------------------------------\n"
      "Sends a single request to a running instance and prints the JSONL reply (and,\n"
      "with --wait, any events) to stdout. Auto-targets the sole running instance; use\n"
      "--pid or --socket to disambiguate. This is the reliable way to drive the channel\n"
      "from a script -- it keeps the connection open until the reply arrives.\n"
      "  microide control-send <command...>          # {\"command\":\"<joined args>\"}\n"
      "  microide control-send --query <verb> [--args <json>]\n"
      "  microide control-send --json '<object>'     # sent verbatim\n"
      "  options: --pid <n> | --socket <path> | --timeout <secs> | --wait <event> | --id <n>\n"
      "Exit codes: 0 reply ok, 1 reply not ok, 2 usage/discovery/connect, 3 --wait timeout.\n"
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
      "Function breakpoints (break by symbol name -- no file/line needed):\n"
      "  breakpoint-function-add <name>\n"
      "  breakpoint-function-remove <name>\n"
      "  breakpoint-function-toggle <name>\n"
      "  breakpoint-function-condition <name> [expr]    (omit expr to clear)\n"
      "  breakpoint-exception-condition <filterId> [expr]\n"
      "Launch a session:\n"
      "  debug-run [--type <adapter>] <program> [args...]  ad-hoc launch by program path\n"
      "                                                    (no pre-defined config needed)\n"
      "  debug-launch [name]                               start a named launch config\n"
      "Debug control: debug-start, debug-continue, debug-step-over, debug-step-in,\n"
      "  debug-step-out, debug-pause, debug-restart, debug-stop. Reverse execution\n"
      "  (recording adapters only): debug-reverse-continue, debug-step-back.\n"
      "  (Over the control channel, any breakpoint-/debug- command auto-enables the\n"
      "  debugger transiently -- no separate debug.enabled prelude is needed.)\n"
      "\n"
      "Recipes (end-to-end agent workflows)\n"
      "------------------------------------\n"
      "1) Open a session, break in a function, hand the live window to the human:\n"
      "   microide /path/to/project --set control.enabled true &   # normal window + socket\n"
      "   microide control-send breakpoint-function-add A          # debugger auto-enables\n"
      "   microide control-send debug-run ./build/app              # or: debug-launch <config>\n"
      "   # leave the window open -- the human now drives it interactively.\n"
      "   (--set control.enabled true opens a NORMAL interactive window with the socket\n"
      "    live; that is the path for \"give me control\". --control, by contrast, is the\n"
      "    headless stream below.)\n"
      "2) Investigate a crash, break just before the suspect line (headless):\n"
      "   microide /path/to/project --control --control-spec spec.json\n"
      "   # spec: {\"breakpoints\":[{\"file\":\"src/foo.c\",\"line\":N}],\"launch\":\"<config>\"}\n"
      "   # the debugger auto-enables; read launch names first via the launch-configs\n"
      "   # query. Watch stdout for {\"event\":\"stopped\",..} then debug-continue/step.\n"
      "   # (functionBreakpoints in the spec break by symbol name with no file/line.)\n"
      "\n"
      "Cold-start spec (microide --control-spec <file.json>)\n"
      "-----------------------------------------------------\n"
      "Open a project with breakpoints already set before the window is interactive:\n"
      "  {\n"
      "    \"project\": \"/path/to/project\",          // optional; selects the project\n"
      "    \"settings\": [[\"control.enabled\",\"true\"]], // optional transient overrides\n"
      "    \"breakpoints\": [\n"
      "      {\"file\":\"src/main.cpp\",\"line\":42},\n"
      "      {\"file\":\"src/util.cpp\",\"line\":120,\"condition\":\"x>10\"},\n"
      "      {\"file\":\"src/log.cpp\",\"line\":7,\"logMessage\":\"hit {x}\"},\n"
      "      {\"file\":\"src/a.cpp\",\"line\":3,\"hitCondition\":\">=5\",\"enabled\":false}\n"
      "    ],\n"
      "    \"functionBreakpoints\": [                  // optional; break by symbol name\n"
      "      {\"name\":\"main\"},\n"
      "      {\"name\":\"process\",\"condition\":\"n>3\",\"enabled\":false}\n"
      "    ],\n"
      "    \"open\": [\"src/main.cpp\"],               // optional files to reveal\n"
      "    \"launch\": \"Python: main\",                // optional launch-config to auto-start\n"
      "    \"commands\": [\"sidebar-hide\"]            // optional raw command lines, run last\n"
      "  }\n"
      "Spec line numbers are 1-based; relative file paths resolve against the project\n"
      "root. `settings` apply transiently and first; the debugger auto-enables when the\n"
      "spec has breakpoints or a launch. `settings` also accepts an object form\n"
      "({\"id\":\"value\"}). Discover names without reading plugin source via the\n"
      "`launch-configs` and `adapters` queries.\n";
  return text;
}

}  // namespace microide::workspace
