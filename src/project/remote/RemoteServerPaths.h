#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "project/remote/RemoteProtocol.h"

namespace microide::project::remote {

// Where the server's socket lives, and the checks that decide whether it may live
// there (dev-docs/design/remote-projects.md § 6.6). Under the user's home rather
// than $XDG_RUNTIME_DIR, which logind removes at logout for a user without linger —
// and an unprivileged user cannot enable linger.

// `<home>/.local/state/microide/server`. `home` is canonicalized first: a system
// that symlinks /home (an ostree host's /home -> /var/home) is the administrator's
// decision, made once; what is verified strictly is what the user owns below it.
std::filesystem::path DefaultServerSocketDir(const std::filesystem::path& home);
std::filesystem::path ServerSocketPath(const std::filesystem::path& dir);
std::filesystem::path ServerLogPath(const std::filesystem::path& dir);

// Create `dir` (and missing parents, mode 0700) and verify it the way tmux verifies
// its socket directory, on every bind: every component is checked WITHOUT following
// symlinks — a symlinked component is refused and named — each is owned by this
// user or root and not writable by group or others (a sticky directory such as
// /tmp excepted), and `dir` itself is owned by this user with mode exactly 0700.
// A directory that fails is never adopted or "fixed"; *error says which component,
// its mode and the expected mode. The resulting socket path must also fit AF_UNIX.
bool EnsureServerSocketDir(const std::filesystem::path& dir, std::string* error);

// 128 random bits as hex, unique to one server process: a resume token from a
// different epoch is refused rather than interpreted.
std::string MakeDaemonEpoch();

// Read logind's KillUserProcesses (logind.conf, then its drop-ins in order, last
// assignment wins) and the user's linger flag, under `root` ("/" in production, a
// fixture in tests). Never SysV IPC or shared memory — only files.
SessionSurvival ReadSessionSurvival(const std::filesystem::path& root, std::string_view user);

}  // namespace microide::project::remote
