#pragma once

#include <filesystem>
#include <vector>

namespace microide::platform {

// Resource ceilings applied to a sandboxed child via setrlimit. Zero means "leave the inherited
// limit untouched". Portable across Linux and macOS.
struct SubprocessResourceLimits {
  long cpu_seconds = 0;          // RLIMIT_CPU
  long address_space_bytes = 0;  // RLIMIT_AS
  long open_files = 0;           // RLIMIT_NOFILE
  long file_size_bytes = 0;      // RLIMIT_FSIZE
};

// Kernel-level confinement for a spawned child, populated from the calling plugin's
// capabilities. `read_roots` / `write_roots` are the plugin-specific filesystem scopes (e.g. the
// project root and the plugin data dir); the platform layer adds the standard system roots a
// process needs to exec and link. When `allow_network` is false the child is blocked from
// creating IPv4/IPv6 sockets.
struct SubprocessSandbox {
  bool enabled = false;
  std::vector<std::filesystem::path> read_roots;
  std::vector<std::filesystem::path> write_roots;
  bool allow_network = true;
  SubprocessResourceLimits limits;
};

// Applies `sandbox` inside a just-forked child, after chdir/env setup and immediately before
// execvp. Order: resource limits, then (Linux) a Landlock filesystem ruleset that confines writes
// to the write roots while leaving the system readable/executable, then an optional seccomp filter
// blocking IPv4/IPv6 sockets. Best-effort and fail-open: on a kernel lacking Landlock/seccomp the
// unavailable layer is skipped rather than aborting the exec, because the in-process capability
// gate remains the primary boundary and this layer is defense-in-depth. Safe to call with
// `enabled == false` (no-op).
void ApplyChildSandbox(const SubprocessSandbox& sandbox);

}  // namespace microide::platform
