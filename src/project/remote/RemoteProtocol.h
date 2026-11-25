#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "util/ContentHash.h"
#include "util/JsonValue.h"

namespace microide::project::remote {

inline constexpr std::int64_t kProtocolVersion = 5;
inline constexpr std::int64_t kMinProtocolVersion = 3;

namespace method {
inline constexpr std::string_view kServerHello = "server/hello";
inline constexpr std::string_view kServerShutdown = "server/shutdown";
inline constexpr std::string_view kServerStatus = "server/status";
inline constexpr std::string_view kActivitySubscribe = "activity/subscribe";
inline constexpr std::string_view kActivityUnsubscribe = "activity/unsubscribe";
inline constexpr std::string_view kActivityChanged = "activity/changed";
inline constexpr std::string_view kProcEnd = "proc/end";
inline constexpr std::string_view kLinkPing = "link/ping";
inline constexpr std::string_view kOpCancel = "op/cancel";
inline constexpr std::string_view kTermOpen = "term/open";
inline constexpr std::string_view kTermAttach = "term/attach";
inline constexpr std::string_view kTermResize = "term/resize";
inline constexpr std::string_view kTermClose = "term/close";
inline constexpr std::string_view kTermDetach = "term/detach";
inline constexpr std::string_view kTermAck = "term/ack";
inline constexpr std::string_view kTermOpenUrl = "term/open-url";
inline constexpr std::string_view kTermEdit = "term/edit";
inline constexpr std::string_view kTermEditDone = "term/edit-done";
inline constexpr std::string_view kTreeManifest = "tree/manifest";
inline constexpr std::string_view kObjectFetch = "object/fetch";
inline constexpr std::string_view kFileWrite = "file/write";
inline constexpr std::string_view kFsOp = "fs/op";
inline constexpr std::string_view kFileUpload = "file/upload";
inline constexpr std::string_view kUploadCapability = "upload";
inline constexpr std::string_view kStatePut = "state/put";
inline constexpr std::string_view kStateGet = "state/get";
inline constexpr std::string_view kStateCapability = "state";
inline constexpr std::size_t kMaxWorkspaceStateBytes = 512u * 1024;
bool IsWorkspaceStateKey(std::string_view key);
inline constexpr std::string_view kWatchSubscribe = "watch/subscribe";
inline constexpr std::string_view kWatchMoved = "watch/moved";
inline constexpr std::string_view kFileRead = "file/read";
inline constexpr std::string_view kServerLog = "server/log";
inline constexpr std::string_view kPortsSubscribe = "ports/subscribe";
inline constexpr std::string_view kPortsListening = "ports/listening";
}

struct Precondition {
  enum class Kind {
    Hash,
    Absent,
    Any,
    Replace,
  };
  Kind kind = Kind::Absent;
  util::ContentHash hash;

  static Precondition Of(const util::ContentHash& hash) { return Precondition{Kind::Hash, hash}; }
  static Precondition NotThere() { return Precondition{Kind::Absent, {}}; }
  static Precondition Anything() { return Precondition{Kind::Any, {}}; }
  static Precondition Replacing() { return Precondition{Kind::Replace, {}}; }
  friend bool operator==(const Precondition&, const Precondition&) = default;
};
util::JsonValue ToJson(const Precondition& precondition);
std::optional<Precondition> PreconditionFromJson(const util::JsonValue& json);

inline constexpr std::int64_t kErrorProtocol = -32600;
inline constexpr std::int64_t kErrorUnknownMethod = -32601;
inline constexpr std::int64_t kErrorInvalidParams = -32602;
inline constexpr std::int64_t kErrorCancelled = -32800;
inline constexpr std::int64_t kErrorIncompatible = -32001;
inline constexpr std::int64_t kErrorPathRefused = -32002;

struct SessionSurvival {
  bool kill_user_processes = false;
  bool linger = false;
};

struct HelloSettings {
  std::int64_t backfill_inflight_bytes = 0;
  std::int64_t max_manifest_files = 0;
};
inline constexpr std::int64_t kDefaultMaxManifestFiles = 500'000;
inline constexpr std::int64_t kMaxManifestFilesCeiling = 2'000'000;
std::int64_t EffectiveMaxManifestFiles(std::int64_t requested);
std::int64_t EffectiveBackfillInflightBytes(std::int64_t requested);

struct HelloRequest {
  std::int64_t protocol = kProtocolVersion;
  std::int64_t min_protocol = kMinProtocolVersion;
  std::string release;
  std::string root;
  HelloSettings settings;
};

struct PortRange {
  std::uint16_t low = 32768;
  std::uint16_t high = 60999;
  bool Contains(std::uint16_t port) const { return port >= low && port <= high; }
  bool operator==(const PortRange&) const = default;
};

struct HelloReply {
  std::int64_t protocol = kProtocolVersion;
  std::int64_t min_protocol = kMinProtocolVersion;
  std::string release;
  std::string daemon_epoch;
  std::vector<std::string> capabilities;
  SessionSurvival session_survival;
  HelloSettings settings;
  std::optional<PortRange> ephemeral_ports;
};

util::JsonValue ToJson(const HelloRequest& hello);
util::JsonValue ToJson(const HelloReply& hello);
std::optional<HelloRequest> HelloRequestFromJson(const util::JsonValue& json, std::string* error);
std::optional<HelloReply> HelloReplyFromJson(const util::JsonValue& json, std::string* error);

struct ActivityFigures {
  std::optional<std::int64_t> rss_bytes;
  std::optional<std::int32_t> cpu_permille;
  bool operator==(const ActivityFigures&) const = default;
};
struct ServerTerminal {
  std::uint64_t handle = 0;
  std::string root;
  std::string cwd;
  std::string command;
  std::size_t clients = 0;
  bool exited = false;
  std::optional<int> pid;
  std::optional<std::size_t> scrollback_lines;
  ActivityFigures figures;
  bool operator==(const ServerTerminal&) const = default;
};
struct ServerProcess {
  std::uint64_t handle = 0;
  std::string root;
  std::vector<std::string> argv;
  std::optional<int> pid;
  std::string cwd;
  bool kept = false;
  std::optional<std::chrono::milliseconds> detached_for;
  bool exited = false;
  std::size_t buffered_bytes = 0;
  ActivityFigures figures;
  bool operator==(const ServerProcess&) const = default;
};
struct ServerWorkspace {
  std::string root;
  std::size_t clients = 0;
  std::size_t terminals = 0;
  std::size_t processes = 0;
  std::optional<std::size_t> files;
  std::optional<std::size_t> hashed;
  std::string watch;
  std::optional<std::int64_t> disk_free;
  std::optional<std::int64_t> disk_total;
  bool operator==(const ServerWorkspace&) const = default;
};
struct ServerStatus {
  std::string daemon_epoch;
  std::string release;
  std::optional<int> pid;
  std::optional<SessionSurvival> session_survival;
  std::optional<std::chrono::milliseconds> uptime;
  std::size_t activity_subscribers = 0;
  ActivityFigures server;
  std::vector<ServerWorkspace> workspaces;
  std::optional<std::vector<ServerTerminal>> terminals;
  std::optional<std::vector<ServerProcess>> processes;
};
inline constexpr std::size_t kMaxListedTerminals = 256;
inline constexpr std::size_t kMaxListedProcesses = 1024;
inline constexpr std::size_t kMaxListedWorkspaces = 256;
inline constexpr std::chrono::milliseconds kActivityInterval{2000};
ServerStatus ServerStatusFromJson(const util::JsonValue& json);

std::optional<std::string> CheckProtocolCompatibility(std::int64_t local_protocol,
                                                      std::int64_t local_min_protocol,
                                                      std::string_view local_release,
                                                      std::int64_t remote_protocol,
                                                      std::int64_t remote_min_protocol,
                                                      std::string_view remote_release);

bool ReleaseIsOlder(std::string_view a, std::string_view b);

bool IsGitMutation(const std::vector<std::string>& argv);

}
