#include "workspace/ControlSpec.h"

#include <array>
#include <unordered_set>

#include "util/JsonValue.h"
#include "workspace/WorkspaceCommandParsing.h"

namespace microide::workspace {

namespace {

constexpr std::array<std::string_view, 7> kSpecKeys = {
    "project", "settings",   "breakpoints", "functionBreakpoints",
    "open",    "launch",     "commands",
};

// Resolve an authored path against the project root. Absolute paths pass
// through; relative paths join the root. Normalized so the breakpoint-store key
// matches what editor surfaces produce.
std::filesystem::path ResolveAgainstProject(const std::filesystem::path& project_root,
                                            const std::filesystem::path& path) {
  std::filesystem::path resolved = path.is_absolute() ? path : project_root / path;
  return resolved.lexically_normal();
}

std::optional<std::string> OptionalString(const util::JsonValue& value) {
  if (value.IsString() && !value.AsString().empty()) {
    return value.AsString();
  }
  return std::nullopt;
}

}  // namespace

std::span<const std::string_view> ControlSpecKeys() {
  return {kSpecKeys.data(), kSpecKeys.size()};
}

ControlSpec ParseControlSpec(std::string_view json) {
  ControlSpec spec;
  std::optional<util::JsonValue> parsed = util::ParseJson(json);
  if (!parsed) {
    spec.parse_error = "invalid JSON";
    return spec;
  }
  if (!parsed->IsObject()) {
    spec.parse_error = "spec must be a JSON object";
    return spec;
  }

  if (parsed->HasKey("project")) {
    if (std::optional<std::string> project = OptionalString((*parsed)["project"])) {
      spec.project = std::filesystem::path(*project);
    } else if (!(*parsed)["project"].IsNull()) {
      spec.parse_error = "\"project\" must be a non-empty string";
      return spec;
    }
  }

  if (parsed->HasKey("settings")) {
    // Two accepted forms: an object {"id":"value",...} or an array of [id, value]
    // string pairs (preserves order). Values must be strings.
    const util::JsonValue& settings = (*parsed)["settings"];
    if (settings.IsObject()) {
      for (const auto& [id, value] : settings.AsObject()) {
        if (!value.IsString()) {
          spec.parse_error = "\"settings\" values must be strings";
          return spec;
        }
        spec.settings.emplace_back(id, value.AsString());
      }
    } else if (settings.IsArray()) {
      for (const util::JsonValue& entry : settings.AsArray()) {
        const util::JsonArray& pair = entry.AsArray();
        if (!entry.IsArray() || pair.size() != 2 || !pair[0].IsString() || !pair[1].IsString() ||
            pair[0].AsString().empty()) {
          spec.parse_error = "\"settings\" array entries must be [\"id\", \"value\"] string pairs";
          return spec;
        }
        spec.settings.emplace_back(pair[0].AsString(), pair[1].AsString());
      }
    } else {
      spec.parse_error = "\"settings\" must be an object or an array of [id, value] pairs";
      return spec;
    }
  }

  if (parsed->HasKey("breakpoints")) {
    const util::JsonValue& breakpoints = (*parsed)["breakpoints"];
    if (!breakpoints.IsArray()) {
      spec.parse_error = "\"breakpoints\" must be an array";
      return spec;
    }
    for (const util::JsonValue& entry : breakpoints.AsArray()) {
      if (!entry.IsObject()) {
        spec.parse_error = "each breakpoint must be an object";
        return spec;
      }
      ControlSpecBreakpoint breakpoint;
      breakpoint.file = entry["file"].AsString();
      if (breakpoint.file.empty()) {
        spec.parse_error = "breakpoint \"file\" must be a non-empty string";
        return spec;
      }
      if (!entry["line"].IsInt() || entry["line"].AsInt() < 1) {
        spec.parse_error = "breakpoint \"line\" must be an integer >= 1";
        return spec;
      }
      breakpoint.line = static_cast<std::size_t>(entry["line"].AsInt());
      if (entry.HasKey("enabled")) {
        breakpoint.enabled = entry["enabled"].AsBool(true);
      }
      breakpoint.condition = OptionalString(entry["condition"]);
      breakpoint.hit_condition = OptionalString(entry["hitCondition"]);
      breakpoint.log_message = OptionalString(entry["logMessage"]);
      spec.breakpoints.push_back(std::move(breakpoint));
    }
  }

  if (parsed->HasKey("functionBreakpoints")) {
    const util::JsonValue& function_breakpoints = (*parsed)["functionBreakpoints"];
    if (!function_breakpoints.IsArray()) {
      spec.parse_error = "\"functionBreakpoints\" must be an array";
      return spec;
    }
    for (const util::JsonValue& entry : function_breakpoints.AsArray()) {
      if (!entry.IsObject()) {
        spec.parse_error = "each function breakpoint must be an object";
        return spec;
      }
      ControlSpecFunctionBreakpoint function_breakpoint;
      function_breakpoint.name = entry["name"].AsString();
      if (function_breakpoint.name.empty()) {
        spec.parse_error = "function breakpoint \"name\" must be a non-empty string";
        return spec;
      }
      if (entry.HasKey("enabled")) {
        function_breakpoint.enabled = entry["enabled"].AsBool(true);
      }
      function_breakpoint.condition = OptionalString(entry["condition"]);
      spec.function_breakpoints.push_back(std::move(function_breakpoint));
    }
  }

  if (parsed->HasKey("open")) {
    const util::JsonValue& open = (*parsed)["open"];
    if (!open.IsArray()) {
      spec.parse_error = "\"open\" must be an array of strings";
      return spec;
    }
    for (const util::JsonValue& entry : open.AsArray()) {
      if (!entry.IsString() || entry.AsString().empty()) {
        spec.parse_error = "\"open\" entries must be non-empty strings";
        return spec;
      }
      spec.open.push_back(entry.AsString());
    }
  }

  if (parsed->HasKey("launch")) {
    spec.launch = OptionalString((*parsed)["launch"]);
    if (!spec.launch && !(*parsed)["launch"].IsNull()) {
      spec.parse_error = "\"launch\" must be a non-empty string";
      return spec;
    }
  }

  if (parsed->HasKey("commands")) {
    const util::JsonValue& commands = (*parsed)["commands"];
    if (!commands.IsArray()) {
      spec.parse_error = "\"commands\" must be an array of strings";
      return spec;
    }
    for (const util::JsonValue& entry : commands.AsArray()) {
      if (!entry.IsString() || entry.AsString().empty()) {
        spec.parse_error = "\"commands\" entries must be non-empty strings";
        return spec;
      }
      spec.commands.push_back(entry.AsString());
    }
  }

  spec.valid = true;
  return spec;
}

std::vector<std::string> ControlSpecToCommands(const ControlSpec& spec,
                                               const std::filesystem::path& project_root) {
  std::vector<std::string> commands;
  if (!spec.valid) {
    return commands;
  }

  const auto line_token = [](std::size_t line) { return std::to_string(line); };

  for (const ControlSpecBreakpoint& breakpoint : spec.breakpoints) {
    const std::string file =
        ResolveAgainstProject(project_root, breakpoint.file).generic_string();
    const std::string quoted_file = QuoteCommandArg(file);
    commands.push_back("breakpoint-set " + quoted_file + " " + line_token(breakpoint.line));
    if (breakpoint.condition) {
      commands.push_back("breakpoint-condition " + quoted_file + " " + line_token(breakpoint.line) +
                         " " + QuoteCommandArg(*breakpoint.condition));
    }
    if (breakpoint.hit_condition) {
      commands.push_back("breakpoint-hit-condition " + quoted_file + " " +
                         line_token(breakpoint.line) + " " +
                         QuoteCommandArg(*breakpoint.hit_condition));
    }
    if (breakpoint.log_message) {
      commands.push_back("breakpoint-logmessage " + quoted_file + " " + line_token(breakpoint.line) +
                         " " + QuoteCommandArg(*breakpoint.log_message));
    }
    if (!breakpoint.enabled) {
      commands.push_back("breakpoint-disable " + quoted_file + " " + line_token(breakpoint.line));
    }
  }

  for (const ControlSpecFunctionBreakpoint& function_breakpoint : spec.function_breakpoints) {
    const std::string quoted_name = QuoteCommandArg(function_breakpoint.name);
    commands.push_back("breakpoint-function-add " + quoted_name);
    if (function_breakpoint.condition) {
      commands.push_back("breakpoint-function-condition " + quoted_name + " " +
                         QuoteCommandArg(*function_breakpoint.condition));
    }
    // add defaults to enabled; toggle once to disable.
    if (!function_breakpoint.enabled) {
      commands.push_back("breakpoint-function-toggle " + quoted_name);
    }
  }

  // Reveal files. An explicit `open` list wins; otherwise reveal the distinct
  // breakpoint files and position the first one at its breakpoint line so the
  // developer lands on the bug.
  if (!spec.open.empty()) {
    for (const std::string& path : spec.open) {
      commands.push_back("open " +
                         QuoteCommandArg(ResolveAgainstProject(project_root, path).generic_string()));
    }
  } else if (!spec.breakpoints.empty()) {
    std::unordered_set<std::string> seen;
    bool revealed_first = false;
    for (const ControlSpecBreakpoint& breakpoint : spec.breakpoints) {
      const std::string file =
          ResolveAgainstProject(project_root, breakpoint.file).generic_string();
      if (!seen.insert(file).second) {
        continue;
      }
      commands.push_back("open " + QuoteCommandArg(file));
      if (!revealed_first) {
        commands.push_back("goto " + line_token(breakpoint.line));
        revealed_first = true;
      }
    }
  }

  if (spec.launch) {
    commands.push_back("debug-launch " + QuoteCommandArg(*spec.launch));
  }

  for (const std::string& command : spec.commands) {
    commands.push_back(command);
  }

  return commands;
}

}  // namespace microide::workspace
