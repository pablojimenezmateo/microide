#include "project/remote/RemoteProtocol.h"

#include "util/Parse.h"

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <utility>

namespace microide::project::remote {
namespace {

// Bounds on what a peer may send in the handshake. Generous for real values, small
// enough that a hostile peer cannot make the handshake allocate much.
constexpr std::size_t kMaxReleaseBytes = 64;
constexpr std::size_t kMaxRootBytes = 4096;
constexpr std::size_t kMaxCapabilities = 64;
constexpr std::size_t kMaxCapabilityBytes = 64;
constexpr std::size_t kMaxEpochBytes = 64;

bool ReadVersion(const util::JsonValue& json, std::string_view key, std::int64_t* out,
                 std::string* error) {
  const util::JsonValue& value = json[key];
  if (!value.IsInt() || value.AsInt() < 1 || value.AsInt() > 1'000'000) {
    *error = std::string(key) + " must be a positive integer";
    return false;
  }
  *out = value.AsInt();
  return true;
}

bool ReadString(const util::JsonValue& json, std::string_view key, std::size_t max_bytes,
                bool required, std::string* out, std::string* error) {
  const util::JsonValue& value = json[key];
  if (value.IsNull() && !required) {
    out->clear();
    return true;
  }
  if (!value.IsString() || value.AsString().size() > max_bytes) {
    *error = std::string(key) + " must be a string of at most " + std::to_string(max_bytes) +
             " bytes";
    return false;
  }
  *out = value.AsString();
  return true;
}

util::JsonValue SettingsJson(const HelloSettings& settings) {
  util::JsonObject object;
  object["backfill_inflight_bytes"] = util::JsonValue(settings.backfill_inflight_bytes);
  return util::JsonValue(std::move(object));
}

bool ReadSettings(const util::JsonValue& json, HelloSettings* out, std::string* error) {
  const util::JsonValue& settings = json["settings"];
  if (settings.IsNull()) {
    return true;
  }
  const util::JsonValue& bytes = settings["backfill_inflight_bytes"];
  if (!settings.IsObject() || (!bytes.IsNull() && (!bytes.IsInt() || bytes.AsInt() < 0))) {
    *error = "settings.backfill_inflight_bytes must be a non-negative integer";
    return false;
  }
  out->backfill_inflight_bytes = bytes.IsNull() ? 0 : bytes.AsInt();
  return true;
}

}  // namespace

std::int64_t EffectiveBackfillInflightBytes(std::int64_t requested) {
  constexpr std::int64_t kFloor = 64 * 1024;
  constexpr std::int64_t kCap = 64 * 1024 * 1024;
  return requested <= 0 ? 0 : std::clamp(requested, kFloor, kCap);
}

util::JsonValue ToJson(const HelloRequest& hello) {
  util::JsonObject object;
  object["protocol"] = util::JsonValue(hello.protocol);
  object["min_protocol"] = util::JsonValue(hello.min_protocol);
  object["release"] = util::JsonValue(hello.release);
  object["root"] = util::JsonValue(hello.root);
  object["settings"] = SettingsJson(hello.settings);
  return util::JsonValue(std::move(object));
}

util::JsonValue ToJson(const HelloReply& hello) {
  util::JsonObject object;
  object["protocol"] = util::JsonValue(hello.protocol);
  object["min_protocol"] = util::JsonValue(hello.min_protocol);
  object["release"] = util::JsonValue(hello.release);
  object["daemon_epoch"] = util::JsonValue(hello.daemon_epoch);
  util::JsonArray capabilities;
  for (const std::string& capability : hello.capabilities) {
    capabilities.push_back(util::JsonValue(capability));
  }
  object["capabilities"] = util::JsonValue(std::move(capabilities));
  util::JsonObject survival;
  survival["kill_user_processes"] = util::JsonValue(hello.session_survival.kill_user_processes);
  survival["linger"] = util::JsonValue(hello.session_survival.linger);
  object["session_survival"] = util::JsonValue(std::move(survival));
  object["settings"] = SettingsJson(hello.settings);
  return util::JsonValue(std::move(object));
}

std::optional<HelloRequest> HelloRequestFromJson(const util::JsonValue& json, std::string* error) {
  if (!json.IsObject()) {
    *error = "hello must be an object";
    return std::nullopt;
  }
  HelloRequest hello;
  if (!ReadVersion(json, "protocol", &hello.protocol, error) ||
      !ReadVersion(json, "min_protocol", &hello.min_protocol, error) ||
      !ReadString(json, "release", kMaxReleaseBytes, true, &hello.release, error) ||
      !ReadString(json, "root", kMaxRootBytes, false, &hello.root, error) ||
      !ReadSettings(json, &hello.settings, error)) {
    return std::nullopt;
  }
  if (hello.min_protocol > hello.protocol) {
    *error = "min_protocol is above protocol";
    return std::nullopt;
  }
  return hello;
}

std::optional<HelloReply> HelloReplyFromJson(const util::JsonValue& json, std::string* error) {
  if (!json.IsObject()) {
    *error = "hello reply must be an object";
    return std::nullopt;
  }
  HelloReply hello;
  if (!ReadVersion(json, "protocol", &hello.protocol, error) ||
      !ReadVersion(json, "min_protocol", &hello.min_protocol, error) ||
      !ReadString(json, "release", kMaxReleaseBytes, true, &hello.release, error) ||
      !ReadString(json, "daemon_epoch", kMaxEpochBytes, true, &hello.daemon_epoch, error) ||
      !ReadSettings(json, &hello.settings, error)) {
    return std::nullopt;
  }
  if (hello.min_protocol > hello.protocol) {
    *error = "min_protocol is above protocol";
    return std::nullopt;
  }
  const util::JsonValue& capabilities = json["capabilities"];
  if (!capabilities.IsNull()) {
    if (!capabilities.IsArray() || capabilities.AsArray().size() > kMaxCapabilities) {
      *error = "capabilities must be an array of at most 64 strings";
      return std::nullopt;
    }
    for (const util::JsonValue& capability : capabilities.AsArray()) {
      if (!capability.IsString() || capability.AsString().size() > kMaxCapabilityBytes) {
        *error = "a capability must be a short string";
        return std::nullopt;
      }
      hello.capabilities.push_back(capability.AsString());
    }
  }
  const util::JsonValue& survival = json["session_survival"];
  if (!survival.IsNull()) {
    if (!survival.IsObject()) {
      *error = "session_survival must be an object";
      return std::nullopt;
    }
    hello.session_survival.kill_user_processes = survival["kill_user_processes"].AsBool(false);
    hello.session_survival.linger = survival["linger"].AsBool(false);
  }
  return hello;
}

namespace {

// A handle is a JSON integer; past 2^53 a peer's double would already have lost
// it, so nothing above that is a handle this server could have issued.
constexpr std::int64_t kMaxHandle = std::int64_t{1} << 53;
constexpr std::int64_t kMaxClients = 1 << 20;
constexpr std::int64_t kMaxCount = std::int64_t{1} << 40;
constexpr std::int64_t kMaxPid = 1 << 30;  // Linux's PID_MAX_LIMIT is 2^22
constexpr std::size_t kMaxListedArgs = 64;
constexpr std::size_t kMaxListedArgBytes = 4096;

// A count in [0, max], else not reported.
std::optional<std::int64_t> ReadCount(const util::JsonValue& value, std::int64_t max = kMaxCount) {
  if (!value.IsInt() || value.AsInt() < 0 || value.AsInt() > max) {
    return std::nullopt;
  }
  return value.AsInt();
}

std::optional<int> ReadPid(const util::JsonValue& value) {
  if (!value.IsInt() || value.AsInt() <= 0 || value.AsInt() > kMaxPid) {
    return std::nullopt;
  }
  return static_cast<int>(value.AsInt());
}

std::optional<std::chrono::milliseconds> ReadMilliseconds(const util::JsonValue& value) {
  const std::optional<std::int64_t> ms = ReadCount(value);
  return ms.has_value() ? std::optional(std::chrono::milliseconds(*ms)) : std::nullopt;
}

ActivityFigures ReadFigures(const util::JsonValue& row) {
  ActivityFigures figures;
  if (const std::optional<std::int64_t> rss = ReadCount(row["rss_bytes"], std::int64_t{1} << 52)) {
    figures.rss_bytes = *rss;
  }
  // -1 is the server's "no previous sample yet": not reported, like a missing key.
  // A million permille is a thousand cores.
  if (const std::optional<std::int64_t> cpu = ReadCount(row["cpu_permille"], 1'000'000)) {
    figures.cpu_permille = static_cast<std::int32_t>(*cpu);
  }
  return figures;
}

std::optional<std::uint64_t> ReadHandle(const util::JsonValue& value) {
  if (!value.IsInt() || value.AsInt() <= 0 || value.AsInt() > kMaxHandle) {
    return std::nullopt;
  }
  return static_cast<std::uint64_t>(value.AsInt());
}

std::string ReadPath(const util::JsonValue& value) {
  return value.IsString() && value.AsString().size() <= kMaxRootBytes ? value.AsString() : std::string();
}

}  // namespace

ServerStatus ServerStatusFromJson(const util::JsonValue& json) {
  ServerStatus status;
  if (!json.IsObject()) {
    return status;
  }
  if (const util::JsonValue& epoch = json["daemon_epoch"];
      epoch.IsString() && epoch.AsString().size() <= kMaxEpochBytes) {
    status.daemon_epoch = epoch.AsString();
  }
  if (const util::JsonValue& release = json["release"];
      release.IsString() && release.AsString().size() <= kMaxReleaseBytes) {
    status.release = release.AsString();
  }
  status.pid = ReadPid(json["pid"]);
  if (const util::JsonValue& survival = json["session_survival"]; survival.IsObject()) {
    status.session_survival = SessionSurvival{
        .kill_user_processes = survival["kill_user_processes"].AsBool(false),
        .linger = survival["linger"].AsBool(false)};
  }
  if (const util::JsonValue& server = json["server"]; server.IsObject()) {
    status.uptime = ReadMilliseconds(server["uptime_ms"]);
    status.server = ReadFigures(server);
    status.activity_subscribers =
        static_cast<std::size_t>(ReadCount(server["activity_subscribers"], kMaxClients).value_or(0));
  }
  if (const util::JsonValue& workspaces = json["workspaces"]; workspaces.IsArray()) {
    for (const util::JsonValue& row : workspaces.AsArray()) {
      if (status.workspaces.size() >= kMaxListedWorkspaces) {
        break;
      }
      const util::JsonValue& root = row["root"];
      if (!row.IsObject() || !root.IsString() || root.AsString().size() > kMaxRootBytes) {
        continue;
      }
      ServerWorkspace workspace{.root = root.AsString(),
                                .clients = static_cast<std::size_t>(ReadCount(row["clients"], kMaxClients).value_or(0)),
                                .terminals = static_cast<std::size_t>(ReadCount(row["terminals"]).value_or(0)),
                                .processes = static_cast<std::size_t>(ReadCount(row["processes"]).value_or(0))};
      if (const std::optional<std::int64_t> files = ReadCount(row["files"])) {
        workspace.files = static_cast<std::size_t>(*files);
      }
      if (const std::optional<std::int64_t> hashed = ReadCount(row["hashed"])) {
        workspace.hashed = static_cast<std::size_t>(*hashed);
      }
      if (const util::JsonValue& watch = row["watch"];
          watch.IsString() && (watch.AsString() == "native" || watch.AsString() == "polling" ||
                               watch.AsString() == "none")) {
        workspace.watch = watch.AsString();
      }
      status.workspaces.push_back(std::move(workspace));
    }
  }
  if (const util::JsonValue& terminals = json["terminals"]; terminals.IsArray()) {
    // An array, or absent / the count an older server sends: not listed.
    std::vector<ServerTerminal>& rows = status.terminals.emplace();
    for (const util::JsonValue& row : terminals.AsArray()) {
      if (rows.size() >= kMaxListedTerminals) {
        break;
      }
      const std::optional<std::uint64_t> handle = ReadHandle(row["handle"]);
      const util::JsonValue& root = row["root"];
      const std::optional<std::int64_t> clients = ReadCount(row["clients"], kMaxClients);
      if (!row.IsObject() || !handle.has_value() || !root.IsString() ||
          root.AsString().size() > kMaxRootBytes || !clients.has_value()) {
        continue;
      }
      ServerTerminal terminal{.handle = *handle,
                              .root = root.AsString(),
                              .cwd = ReadPath(row["cwd"]),
                              .clients = static_cast<std::size_t>(*clients),
                              .exited = row["exited"].AsBool(false),
                              .pid = ReadPid(row["pid"]),
                              .figures = ReadFigures(row)};
      if (const std::optional<std::int64_t> lines = ReadCount(row["scrollback_lines"])) {
        terminal.scrollback_lines = static_cast<std::size_t>(*lines);
      }
      rows.push_back(std::move(terminal));
    }
  }
  if (const util::JsonValue& processes = json["processes"]; processes.IsArray()) {
    std::vector<ServerProcess>& rows = status.processes.emplace();
    for (const util::JsonValue& row : processes.AsArray()) {
      if (rows.size() >= kMaxListedProcesses) {
        break;
      }
      const std::optional<std::uint64_t> handle = ReadHandle(row["handle"]);
      const util::JsonValue& root = row["root"];
      const util::JsonValue& argv = row["argv"];
      if (!row.IsObject() || !handle.has_value() || !root.IsString() ||
          root.AsString().size() > kMaxRootBytes || !argv.IsArray()) {
        continue;
      }
      ServerProcess process{.handle = *handle, .root = root.AsString()};
      bool argv_ok = true;
      for (const util::JsonValue& arg : argv.AsArray()) {
        if (!arg.IsString() || arg.AsString().size() > kMaxListedArgBytes) {
          argv_ok = false;
          break;
        }
        if (process.argv.size() < kMaxListedArgs) {
          process.argv.push_back(arg.AsString());
        }
      }
      if (!argv_ok) {
        continue;  // a row naming its process in something other than strings
      }
      process.pid = ReadPid(row["pid"]);
      process.cwd = ReadPath(row["cwd"]);
      process.kept = row["kept"].AsBool(false);
      process.detached_for = ReadMilliseconds(row["detached_ms"]);
      process.exited = row["exited"].AsBool(false);
      process.buffered_bytes = static_cast<std::size_t>(ReadCount(row["buffered_bytes"]).value_or(0));
      process.figures = ReadFigures(row);
      rows.push_back(std::move(process));
    }
  }
  return status;
}

std::optional<std::string> CheckProtocolCompatibility(std::int64_t local_protocol,
                                                      std::int64_t local_min_protocol,
                                                      std::string_view local_release,
                                                      std::int64_t remote_protocol,
                                                      std::int64_t remote_min_protocol,
                                                      std::string_view remote_release) {
  const auto describe = [](std::string_view release, std::int64_t protocol) {
    return std::string(release.empty() ? std::string_view("unknown") : release) +
           " (protocol " + std::to_string(protocol) + ")";
  };
  if (remote_protocol < local_min_protocol) {
    return "the host's microide-server " + describe(remote_release, remote_protocol) +
           " is too old for this microide " + describe(local_release, local_protocol);
  }
  if (local_protocol < remote_min_protocol) {
    return "this microide " + describe(local_release, local_protocol) +
           " is too old for the host's microide-server " +
           describe(remote_release, remote_protocol);
  }
  return std::nullopt;
}

util::JsonValue ToJson(const Precondition& precondition) {
  switch (precondition.kind) {
    case Precondition::Kind::Absent:
      return util::JsonValue(std::string("absent"));
    case Precondition::Kind::Any:
      return util::JsonValue(std::string("any"));
    case Precondition::Kind::Hash:
      break;
  }
  return util::JsonValue(precondition.hash.Hex());
}

std::optional<Precondition> PreconditionFromJson(const util::JsonValue& json) {
  if (!json.IsString()) {
    return std::nullopt;
  }
  const std::string& text = json.AsString();
  if (text == "absent") {
    return Precondition::NotThere();
  }
  if (text == "any") {
    return Precondition::Anything();
  }
  if (const auto hash = util::ContentHash::FromHex(text)) {
    return Precondition::Of(*hash);
  }
  return std::nullopt;
}

namespace {

std::optional<std::vector<std::uint64_t>> ParseRelease(std::string_view text) {
  std::vector<std::uint64_t> parts;
  while (!text.empty()) {
    const std::size_t dot = text.find('.');
    const std::string_view part = text.substr(0, dot);
    const std::optional<std::size_t> value = util::ParseSize(part);
    if (part.empty() || !value.has_value()) {
      return std::nullopt;
    }
    parts.push_back(*value);
    if (dot == std::string_view::npos) {
      break;
    }
    text.remove_prefix(dot + 1);
    if (text.empty()) {
      return std::nullopt;  // a trailing dot
    }
  }
  if (parts.empty()) {
    return std::nullopt;
  }
  return parts;
}

}  // namespace

bool ReleaseIsOlder(std::string_view a, std::string_view b) {
  std::optional<std::vector<std::uint64_t>> left = ParseRelease(a);
  std::optional<std::vector<std::uint64_t>> right = ParseRelease(b);
  if (!left.has_value() || !right.has_value()) {
    return false;
  }
  // Missing trailing parts read as 0: "2.13" == "2.13.0".
  const std::size_t size = std::max(left->size(), right->size());
  left->resize(size, 0);
  right->resize(size, 0);
  return *left < *right;
}

bool IsGitMutation(const std::vector<std::string>& argv) {
  if (argv.empty() || std::filesystem::path(argv.front()).filename() != "git") {
    return false;
  }
  static constexpr std::string_view kReadOnly[] = {
      "status",   "diff",      "log",       "show",        "blame",       "for-each-ref",
      "rev-parse", "rev-list", "ls-files",  "ls-tree",     "cat-file",    "merge-base",
      "show-ref", "describe",  "name-rev",  "check-ignore", "check-attr", "grep",
      "shortlog", "version",   "var",       "count-objects", "diff-tree", "diff-index",
      "diff-files"};
  for (std::size_t i = 1; i < argv.size(); ++i) {
    const std::string& arg = argv[i];
    if (arg == "-C" || arg == "-c" || arg == "--git-dir" || arg == "--work-tree" ||
        arg == "--namespace") {
      ++i;  // a global option's value
      continue;
    }
    if (!arg.empty() && arg.front() == '-') {
      continue;  // --no-pager, --literal-pathspecs, --git-dir=...
    }
    return std::find(std::begin(kReadOnly), std::end(kReadOnly), arg) == std::end(kReadOnly);
  }
  return false;  // bare `git`
}

}  // namespace microide::project::remote
