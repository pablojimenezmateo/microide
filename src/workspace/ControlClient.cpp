#include "workspace/ControlClient.h"

#include <chrono>
#include <iostream>

#include "platform/ControlSocketClient.h"
#include "util/JsonValue.h"
#include "util/Parse.h"
#include "workspace/ControlChannelService.h"

namespace microide::workspace {

namespace {

// Consume the value that must follow a flag like `--pid`. Advances *index past
// the value. Returns nullopt (and flags an error) when the value is missing.
std::optional<std::string> TakeValue(const std::vector<std::string>& args, std::size_t* index,
                                     std::string_view flag, std::string* error) {
  if (*index + 1 >= args.size()) {
    *error = std::string(flag) + " requires a value";
    return std::nullopt;
  }
  ++(*index);
  return args[*index];
}

std::string JoinTokens(const std::vector<std::string>& tokens) {
  std::string joined;
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    if (i != 0) {
      joined.push_back(' ');
    }
    joined += tokens[i];
  }
  return joined;
}

void EmitError(const std::string& message) {
  std::cerr << util::SerializeJson(util::JsonObject{{"error", message}}) << '\n';
}

}  // namespace

ControlSendOptions ParseControlSendArgs(const std::vector<std::string>& args) {
  ControlSendOptions options;
  // Recognized control-send flags are consumed wherever they appear, so an agent
  // can append `--wait`/`--timeout` after the command verb. Any other token
  // (including unknown --flags such as debug-run's --type) forms the command, in
  // order. control-send's own flag names never collide with debug commands.
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& token = args[i];
    if (token == "--pid") {
      const auto value = TakeValue(args, &i, "--pid", &options.error);
      if (!value) return options;
      const std::optional<int> pid = util::ParseInt(*value);
      if (!pid) {
        options.error = "--pid must be an integer";
        return options;
      }
      options.pid = *pid;
    } else if (token == "--socket") {
      const auto value = TakeValue(args, &i, "--socket", &options.error);
      if (!value) return options;
      options.socket = std::filesystem::path(*value);
    } else if (token == "--timeout") {
      const auto value = TakeValue(args, &i, "--timeout", &options.error);
      if (!value) return options;
      const std::optional<int> seconds = util::ParseInt(*value);
      if (!seconds || *seconds <= 0) {
        options.error = "--timeout must be a positive integer (seconds)";
        return options;
      }
      options.timeout_seconds = *seconds;
    } else if (token == "--wait") {
      const auto value = TakeValue(args, &i, "--wait", &options.error);
      if (!value) return options;
      options.wait_event = *value;
    } else if (token == "--id") {
      const auto value = TakeValue(args, &i, "--id", &options.error);
      if (!value) return options;
      const std::optional<int> id = util::ParseInt(*value);
      if (!id) {
        options.error = "--id must be an integer";
        return options;
      }
      options.request_id = *id;
    } else if (token == "--query") {
      const auto value = TakeValue(args, &i, "--query", &options.error);
      if (!value) return options;
      options.query_verb = *value;
    } else if (token == "--args") {
      const auto value = TakeValue(args, &i, "--args", &options.error);
      if (!value) return options;
      options.query_args_json = *value;
    } else if (token == "--json") {
      const auto value = TakeValue(args, &i, "--json", &options.error);
      if (!value) return options;
      options.raw_json = *value;
    } else {
      options.command_tokens.push_back(token);
    }
  }

  // Shape validation: exactly one of command / --query / --json.
  const int shapes = (options.command_tokens.empty() ? 0 : 1) +
                     (options.query_verb.has_value() ? 1 : 0) +
                     (options.raw_json.has_value() ? 1 : 0);
  if (shapes == 0) {
    options.error = "nothing to send: give a command, --query <verb>, or --json <object>";
    return options;
  }
  if (shapes > 1) {
    options.error = "choose exactly one of a command, --query, or --json";
    return options;
  }
  if (options.query_args_json.has_value() && !options.query_verb.has_value()) {
    options.error = "--args is only valid with --query";
    return options;
  }
  if (options.pid.has_value() && options.socket.has_value()) {
    options.error = "choose at most one of --pid or --socket";
    return options;
  }
  options.valid = true;
  return options;
}

std::optional<std::string> BuildControlSendRequest(const ControlSendOptions& options,
                                                   std::string* error) {
  if (options.raw_json.has_value()) {
    const std::optional<util::JsonValue> parsed = util::ParseJson(*options.raw_json);
    if (!parsed || !parsed->IsObject()) {
      if (error) *error = "--json must be a single JSON object";
      return std::nullopt;
    }
    util::JsonObject object = parsed->AsObject();
    if (object.find("id") == object.end()) {
      object["id"] = static_cast<std::int64_t>(options.request_id);
    }
    return util::SerializeJson(util::JsonValue(std::move(object)));
  }

  util::JsonObject object;
  object["id"] = static_cast<std::int64_t>(options.request_id);
  if (options.query_verb.has_value()) {
    object["query"] = *options.query_verb;
    if (options.query_args_json.has_value()) {
      const std::optional<util::JsonValue> args = util::ParseJson(*options.query_args_json);
      if (!args) {
        if (error) *error = "--args must be valid JSON";
        return std::nullopt;
      }
      object["args"] = *args;
    }
  } else {
    object["command"] = JoinTokens(options.command_tokens);
  }
  return util::SerializeJson(util::JsonValue(std::move(object)));
}

#if defined(__unix__) || defined(__APPLE__)

namespace {

// Resolve the target socket from options + discovery. Returns nullopt and sets
// *error on ambiguity / no instance / unknown pid.
std::optional<std::filesystem::path> ResolveSocket(const ControlSendOptions& options,
                                                   std::string* error) {
  if (options.socket.has_value()) {
    return *options.socket;
  }
  const std::vector<ControlInstanceDescriptor> instances = EnumerateControlInstances();
  if (options.pid.has_value()) {
    for (const ControlInstanceDescriptor& instance : instances) {
      if (instance.pid == *options.pid) {
        return instance.socket;
      }
    }
    *error = "no running instance with pid " + std::to_string(*options.pid);
    return std::nullopt;
  }
  if (instances.empty()) {
    *error = "no running microide instance has the control channel enabled "
             "(start one with --set control.enabled true or --control)";
    return std::nullopt;
  }
  if (instances.size() > 1) {
    std::string pids;
    for (const ControlInstanceDescriptor& instance : instances) {
      if (!pids.empty()) pids += ", ";
      pids += std::to_string(instance.pid);
    }
    *error = "multiple instances are running (" + pids + "); select one with --pid";
    return std::nullopt;
  }
  return instances.front().socket;
}

}  // namespace

int RunControlSend(int argc, char** argv) {
  std::vector<std::string> args;
  for (int i = 2; i < argc; ++i) {  // skip argv[0] (binary) and argv[1] (control-send)
    if (argv[i] != nullptr) {
      args.emplace_back(argv[i]);
    }
  }

  const ControlSendOptions options = ParseControlSendArgs(args);
  if (!options.valid) {
    EmitError(options.error);
    return 2;
  }

  std::string build_error;
  const std::optional<std::string> request = BuildControlSendRequest(options, &build_error);
  if (!request) {
    EmitError(build_error);
    return 2;
  }
  // The id we will match the response against (honors --json's own id).
  int response_id = options.request_id;
  if (const std::optional<util::JsonValue> parsed = util::ParseJson(*request);
      parsed && parsed->IsObject() && parsed->HasKey("id")) {
    response_id = static_cast<int>((*parsed)["id"].AsInt(options.request_id));
  }

  std::string resolve_error;
  const std::optional<std::filesystem::path> socket = ResolveSocket(options, &resolve_error);
  if (!socket) {
    EmitError(resolve_error);
    return 2;
  }

  platform::ControlSocketClient client;
  if (!client.Connect(*socket)) {
    EmitError("could not connect to " + socket->string());
    return 2;
  }
  if (!client.SendLine(*request)) {
    EmitError("failed to send request");
    return 2;
  }
  // Half-closing lets the server reap us promptly once it has replied — but only
  // when we are not also waiting for a later broadcast event (which would be lost
  // if the connection were reaped right after the command's reply).
  if (!options.wait_event.has_value()) {
    client.ShutdownWrite();
  }

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(options.timeout_seconds);
  bool response_seen = false;
  bool response_ok = false;
  bool wait_seen = !options.wait_event.has_value();

  while (true) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      break;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    const std::optional<std::string> line = client.ReadLine(remaining);
    if (!line) {
      break;  // timeout or EOF
    }
    std::cout << *line << '\n';
    std::cout.flush();

    const std::optional<util::JsonValue> parsed = util::ParseJson(*line);
    if (!parsed || !parsed->IsObject()) {
      continue;
    }
    if (parsed->HasKey("event")) {
      if (options.wait_event.has_value() &&
          (*parsed)["event"].AsString() == *options.wait_event) {
        // For a two-phase stop, wait for the resolved frame (framesPending:false),
        // not the instant first notification.
        const bool frames_pending = (*parsed)["framesPending"].AsBool(false);
        if (!((*parsed)["event"].AsString() == "stopped" && frames_pending)) {
          wait_seen = true;
        }
      }
    } else if (parsed->HasKey("ok") && static_cast<int>((*parsed)["id"].AsInt(-1)) == response_id) {
      response_seen = true;
      response_ok = (*parsed)["ok"].AsBool(false);
    }

    // A failed command yields no follow-up event, so stop waiting for one.
    if (response_seen && (!response_ok || wait_seen)) {
      break;
    }
  }
  client.Close();

  if (!response_seen) {
    EmitError("timed out waiting for a reply");
    return 2;
  }
  if (!response_ok) {
    return 1;
  }
  if (options.wait_event.has_value() && !wait_seen) {
    EmitError("reply ok, but the '" + *options.wait_event + "' event did not arrive in time");
    return 3;
  }
  return 0;
}

#else  // non-POSIX: control channel unsupported.

int RunControlSend(int, char**) {
  EmitError("the control channel is not supported on this platform");
  return 2;
}

#endif

}  // namespace microide::workspace
