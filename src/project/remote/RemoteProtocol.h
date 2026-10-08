#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "util/JsonValue.h"

namespace microide::project::remote {

// The wire version, independent of the app's release version
// (dev-docs/design/remote-projects.md § 6.4): bumped only when the wire changes,
// with a floor each side still accepts, so a 2.14 client talks to a 2.12 server
// for as long as the wire has not actually moved.
inline constexpr std::int64_t kProtocolVersion = 1;
inline constexpr std::int64_t kMinProtocolVersion = 1;

// Method names. A method not listed here is answered with kErrorUnknownMethod.
namespace method {
inline constexpr std::string_view kServerHello = "server/hello";
inline constexpr std::string_view kServerShutdown = "server/shutdown";
inline constexpr std::string_view kLinkPing = "link/ping";
inline constexpr std::string_view kOpCancel = "op/cancel";  // notification: {"id": <request>}
// Host terminals (§ 6.5). Requests: term/open {cwd, command?, shell?: [argv], rows,
// columns, scrollback_lines} -> {handle, credit_bytes}; term/attach {handle} -> true.
// Notifications: term/resize {handle, rows, columns}, term/close {handle},
// term/ack {handle, bytes} (total frame bytes received). Input and frames are the
// TermInput / TermFrame content frames.
inline constexpr std::string_view kTermOpen = "term/open";
inline constexpr std::string_view kTermAttach = "term/attach";
inline constexpr std::string_view kTermResize = "term/resize";
inline constexpr std::string_view kTermClose = "term/close";
inline constexpr std::string_view kTermAck = "term/ack";
}  // namespace method

// Error codes in a Response's {"error": {"code", "message"}}.
inline constexpr std::int64_t kErrorProtocol = -32600;       // malformed body
inline constexpr std::int64_t kErrorUnknownMethod = -32601;
inline constexpr std::int64_t kErrorInvalidParams = -32602;
inline constexpr std::int64_t kErrorCancelled = -32800;
inline constexpr std::int64_t kErrorIncompatible = -32001;   // version handshake failed

// Whether processes survive the user logging out on the host (§ 6.12): systemd's
// KillUserProcesses and the user's linger flag. Reported, never hidden — a host
// that kills user processes at logout makes "terminals survive a disconnect"
// false, and the user must be told.
struct SessionSurvival {
  bool kill_user_processes = false;
  bool linger = false;
};

struct HelloRequest {
  std::int64_t protocol = kProtocolVersion;
  std::int64_t min_protocol = kMinProtocolVersion;
  std::string release;  // the client's app version, for messages only
  std::string root;     // workspace root on the host ("" = none: a terminal-only session)
};

struct HelloReply {
  std::int64_t protocol = kProtocolVersion;
  std::int64_t min_protocol = kMinProtocolVersion;
  std::string release;
  // Unique per server process. A reattach that finds a different epoch knows its
  // handles are gone (the server restarted) and attaches cold.
  std::string daemon_epoch;
  std::vector<std::string> capabilities;
  SessionSurvival session_survival;
};

util::JsonValue ToJson(const HelloRequest& hello);
util::JsonValue ToJson(const HelloReply& hello);
// Validating decoders: every field is untrusted (§ 6.9). nullopt with *error naming
// the first field that is missing, mistyped or out of range.
std::optional<HelloRequest> HelloRequestFromJson(const util::JsonValue& json, std::string* error);
std::optional<HelloReply> HelloReplyFromJson(const util::JsonValue& json, std::string* error);

// Whether two peers can talk: each side's version must be at least the other's
// floor. nullopt when compatible; otherwise the message the user sees, naming both
// RELEASE versions (what the user knows), and which side is too old.
std::optional<std::string> CheckProtocolCompatibility(std::int64_t local_protocol,
                                                      std::int64_t local_min_protocol,
                                                      std::string_view local_release,
                                                      std::int64_t remote_protocol,
                                                      std::int64_t remote_min_protocol,
                                                      std::string_view remote_release);

}  // namespace microide::project::remote
