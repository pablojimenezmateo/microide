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
// server/status {activity?: bool} -> {release, pid, daemon_epoch, session_survival,
// server: {uptime_ms, activity_subscribers}, workspaces: [{root, clients, terminals, processes,
// files?, hashed?, watch?}], terminals: [{handle, root, cwd, clients, exited, pid,
// scrollback_lines}], processes: [{handle, root, argv, pid, cwd, kept, detached_ms?,
// exited, buffered_bytes}], live_processes}: what `microide-server status` prints.
// With `activity` the server row and every terminal and process row also carry
// {rss_bytes, cpu_permille} (cpu_permille -1 on a pid's first sample), from one
// /proc walk (remote-host-activity D1). Before the listings `terminals` and
// `processes` were counts; a client reads that as "not listed" (ServerStatusFromJson).
inline constexpr std::string_view kServerStatus = "server/status";
// activity/subscribe {} -> the server/status reply with activity; then, every
// kActivityInterval while subscribed, an activity/changed notification carrying
// the same. activity/unsubscribe {} (a notification), or the connection closing,
// stops it; the server samples nothing while nobody is subscribed (D2).
inline constexpr std::string_view kActivitySubscribe = "activity/subscribe";
inline constexpr std::string_view kActivityUnsubscribe = "activity/unsubscribe";
inline constexpr std::string_view kActivityChanged = "activity/changed";
// proc/end {handle} (a notification): the Host view's End. The server terminates
// the process (SIGTERM to its group, SIGKILL after a grace) whoever spawned it,
// and forgets it once it exited with no client holding it -- a kept process
// nobody is attached to would otherwise wait for a release that never comes.
inline constexpr std::string_view kProcEnd = "proc/end";
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
// watch/moved {manifest} notification, on the bulk lane just before that manifest's
// WatchDelta frames: the host repository's HEAD moved since the previous batch (a
// checkout, a reset, a commit). The client follows a mass delete that comes with
// one instead of holding it (§ 6.3). A peer that does not know it drops it.
inline constexpr std::string_view kWatchMoved = "watch/moved";
// file/read {path} -> ObjectData frames (index 0) then {objects: [one answer]}, as
// object/fetch: a READ-ONLY read of an absolute host path outside the content set —
// a system header a language server names, a file in an ignored build/ directory.
// Not root-confined: it reads what the user could read on the host anyway (§ 6.9).
inline constexpr std::string_view kFileRead = "file/read";
// server/log {} -> {path, text}: the tail (at most 256 KiB) of the server's log
// beside its socket; empty text for a server with no log (serve-stdio).
inline constexpr std::string_view kServerLog = "server/log";
// ports/subscribe {} -> {ports: [n]}: the TCP ports this user listens on at the
// host's loopback (or every address), then a ports/listening {ports: [n]}
// notification whenever that set changes (sampled every 2 s): the client forwards
// a port a host program starts listening on (VS Code's auto forward).
inline constexpr std::string_view kPortsSubscribe = "ports/subscribe";
inline constexpr std::string_view kPortsListening = "ports/listening";
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
  std::string root;     // workspace root on the host ("" = none: a terminal-only session,
                        // which the server still accepts from older clients)
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

// The part of a server/status reply the client acts on. Every field is untrusted
// and nothing fails the decode: a missing or mistyped field reads as not reported,
// never as a wrong value.
// What a row costs on the host, when the reply carries activity. Either half may
// be missing: a server older than the figures, a reply that did not ask, a pid
// that was gone by the sample, and -- for the CPU share -- a pid's first sample,
// which has nothing to take a difference against.
struct ActivityFigures {
  std::optional<std::int64_t> rss_bytes;
  std::optional<std::int32_t> cpu_permille;  // thousandths of one core
  bool operator==(const ActivityFigures&) const = default;
};
struct ServerTerminal {
  std::uint64_t handle = 0;  // never 0 once decoded
  std::string root;          // the opening connection's workspace root ("" = none)
  std::string cwd;           // "" = not reported
  std::size_t clients = 0;   // attached connections
  bool exited = false;
  std::optional<int> pid;  // the shell's
  std::optional<std::size_t> scrollback_lines;
  ActivityFigures figures;  // of the shell and everything under it
  bool operator==(const ServerTerminal&) const = default;
};
struct ServerProcess {
  std::uint64_t handle = 0;  // never 0 once decoded
  std::string root;          // the spawning connection's workspace root ("" = none)
  std::vector<std::string> argv;
  std::optional<int> pid;
  std::string cwd;
  bool kept = false;
  std::optional<std::chrono::milliseconds> detached_for;  // set while no client holds it
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
  std::string watch;  // "native", "polling", "none"; "" = not reported
  bool operator==(const ServerWorkspace&) const = default;
};
struct ServerStatus {
  std::string daemon_epoch;  // "" = not reported
  std::string release;       // "" = not reported
  std::optional<int> pid;
  std::optional<SessionSurvival> session_survival;
  std::optional<std::chrono::milliseconds> uptime;
  std::size_t activity_subscribers = 0;  // connections the sampler runs for
  ActivityFigures server;                         // the server process alone
  std::vector<ServerWorkspace> workspaces;
  // nullopt: the server lists no terminals (one older than the listing, whose
  // `terminals` is a count). A row with an unusable handle, root or client count
  // is dropped rather than guessed at.
  std::optional<std::vector<ServerTerminal>> terminals;
  // nullopt: the server lists no processes (older than remote-host-activity). Rows
  // as for terminals.
  std::optional<std::vector<ServerProcess>> processes;
};
// At most this many rows are kept from one reply (the server caps its own tables
// at 64 terminals and 256 processes).
inline constexpr std::size_t kMaxListedTerminals = 256;
inline constexpr std::size_t kMaxListedProcesses = 1024;
inline constexpr std::size_t kMaxListedWorkspaces = 256;
// How often a subscribed server pushes activity/changed.
inline constexpr std::chrono::milliseconds kActivityInterval{2000};
ServerStatus ServerStatusFromJson(const util::JsonValue& json);

// Whether two peers can talk: each side's version must be at least the other's
// floor. nullopt when compatible; otherwise the message the user sees, naming both
// RELEASE versions (what the user knows), and which side is too old.
std::optional<std::string> CheckProtocolCompatibility(std::int64_t local_protocol,
                                                      std::int64_t local_min_protocol,
                                                      std::string_view local_release,
                                                      std::int64_t remote_protocol,
                                                      std::int64_t remote_min_protocol,
                                                      std::string_view remote_release);

// Whether release `a` is strictly older than `b`, comparing dotted numeric
// versions ("2.12.0" < "2.13.0" < "2.13.1"). False when either is not of that form
// (a dev build, a test's "test"): only a version both sides can order may trigger
// an update, so an older editor never downgrades a host's server.
bool ReleaseIsOlder(std::string_view a, std::string_view b);

// Whether `argv` is a git command that may change a repository. Conservative: a
// subcommand not known to be read-only counts. Both ends use it: the client to know
// when the pushed git status goes stale until the host pushes one computed after
// the command, the server to know which exits call for that push -- a read-only
// git (the sidebar's own `git diff`) must not, or each refresh triggers the next.
bool IsGitMutation(const std::vector<std::string>& argv);

}  // namespace microide::project::remote
