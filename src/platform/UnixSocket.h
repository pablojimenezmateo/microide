#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

namespace microide::platform {

// The AF_UNIX address limit (sun_path minus its NUL). A longer path does not bind;
// callers name the limit and the length rather than failing silently.
inline constexpr std::size_t kMaxUnixSocketPathBytes = 107;

// Connect to the stream socket at `path`: a blocking connect, the descriptor
// close-on-exec from creation. -1 when it cannot (absent, refused, too long).
int ConnectUnixSocket(const std::filesystem::path& path);

// Bind and listen on `path`, mode 0600, non-blocking and close-on-exec. A stale
// socket at the path is removed first — but only a SOCKET owned by this user; a
// regular file, a directory, a symlink or someone else's socket makes this fail
// rather than delete it. -1 with *error naming the reason.
int ListenUnixSocket(const std::filesystem::path& path, std::string* error);

// The stale-socket rule ListenUnixSocket applies: true when `path` is clear
// (absent, or a socket of ours that was removed).
bool ClearStaleSocketPath(const std::filesystem::path& path);

}  // namespace microide::platform
