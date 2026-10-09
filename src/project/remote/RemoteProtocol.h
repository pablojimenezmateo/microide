#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "util/ContentHash.h"
#include "util/JsonValue.h"

namespace microide::project::remote {

// The wire version, independent of the app's release version
// (dev-docs/design/remote-projects.md § 6.4): bumped only when the wire changes,
// with a floor each side still accepts, so a 2.14 client talks to a 2.12 server
// for as long as the wire has not actually moved.
// 3 (2026-10-09): host terminal frames carry OSC 8 links and OSC 9/777
// notifications, and a cell style run may carry a link id; the hello carries the
// settings the server consumes and echoes them as applied.
inline constexpr std::int64_t kProtocolVersion = 3;
inline constexpr std::int64_t kMinProtocolVersion = 3;

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
// The workspace tree (Phase 2b, § 6.2). tree/manifest {} -> TreeRows content frames
// (id = the request) on the bulk lane, then, on the same lane so it cannot overtake
// them, {manifest_id, rows, git}. Needs a hello with a root.
inline constexpr std::string_view kTreeManifest = "tree/manifest";
// object/fetch {objects: [{path}], max_bytes} -> ObjectData frames (id = request,
// payload = varint index + bytes, in order per object) on the request's lane, then
// on that lane {objects: [{hash} | {missing: true} | {error}]}, the hash being of
// exactly the bytes sent.
inline constexpr std::string_view kObjectFetch = "object/fetch";
// file/write {path, expect, mode?} after its WriteData frames -> {hash} or
// {conflict: true, current: hash | null}.
inline constexpr std::string_view kFileWrite = "file/write";
// fs/op {op: "mkdir"|"rename"|"delete", path, to?, expect?} -> {} or
// {conflict: true, current: hash | null}.
inline constexpr std::string_view kFsOp = "fs/op";
// watch/subscribe {} -> {native: bool}. From the next tree/manifest on, every change
// to the content set arrives as WatchDelta frames against what this connection was
// last sent. Subscribe first, then fetch the manifest: changes in between are in
// the manifest, and nothing is sent before it.
inline constexpr std::string_view kWatchSubscribe = "watch/subscribe";
// file/read {path} -> ObjectData frames (index 0) then {objects: [one answer]}, as
// object/fetch: a READ-ONLY read of an absolute host path outside the content set —
// a system header a language server names, a file in an ignored build/ directory.
// Not root-confined: it reads what the user could read on the host anyway (§ 6.9).
inline constexpr std::string_view kFileRead = "file/read";
// server/log {} -> {path, text}: the tail (at most 256 KiB) of the server's log
// beside its socket; empty text for a server with no log (serve-stdio).
inline constexpr std::string_view kServerLog = "server/log";
}  // namespace method

// What a write or tree operation requires of the path's current content (§ 6.3):
// a hash (an update), absent (a create: O_EXCL / RENAME_NOREPLACE), or any (the
// user's explicit Overwrite, never a default). On the wire: "absent", "any", or
// the hash's 64 hex digits.
struct Precondition {
  enum class Kind {
    Hash,
    Absent,
    Any,
  };
  Kind kind = Kind::Absent;
  util::ContentHash hash;

  static Precondition Of(const util::ContentHash& hash) { return Precondition{Kind::Hash, hash}; }
  static Precondition NotThere() { return Precondition{Kind::Absent, {}}; }
  static Precondition Anything() { return Precondition{Kind::Any, {}}; }
  friend bool operator==(const Precondition&, const Precondition&) = default;
};
util::JsonValue ToJson(const Precondition& precondition);
std::optional<Precondition> PreconditionFromJson(const util::JsonValue& json);

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

// The settings the server consumes (§ 6.12): the client sends its values in the
// hello, and the reply echoes what the server actually applies.
struct HelloSettings {
  // `remote.backfill_inflight_bytes`: bulk bytes either side may have
  // unacknowledged. 0 = adaptive (~100 ms of measured bandwidth, 64 KiB..1 MiB).
  std::int64_t backfill_inflight_bytes = 0;
};
// The bound a requested `backfill_inflight_bytes` resolves to: 0 stays adaptive;
// anything else is clamped to [64 KiB, 64 MiB] — below one bulk chunk nothing
// moves, and above that the bound no longer bounds an echo's wait.
std::int64_t EffectiveBackfillInflightBytes(std::int64_t requested);

struct HelloRequest {
  std::int64_t protocol = kProtocolVersion;
  std::int64_t min_protocol = kMinProtocolVersion;
  std::string release;  // the client's app version, for messages only
  std::string root;     // workspace root on the host ("" = none: a terminal-only session)
  HelloSettings settings;
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
  HelloSettings settings;  // effective, as the server applies them
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
