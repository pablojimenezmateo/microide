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
  // Single FILES the child may read (and nothing beside them), for the few that
  // live in a directory the child must not otherwise see -- the user's git
  // configuration in $HOME. A directory here grants nothing.
  std::vector<std::filesystem::path> read_files;
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

// The user's git configuration, which git -- and every tool that shells out to it --
// reads on start and treats as FATAL when it exists but cannot be read ("unable to
// access ~/.gitconfig: Permission denied"). Configuration, not credentials:
// ~/.git-credentials and ~/.ssh are deliberately not here. Appends the global config
// file to `read_files` and the XDG git directory to `read_roots`; a path that does
// not exist is simply never granted. Reads HOME and XDG_CONFIG_HOME, so call it in
// the PARENT when building the sandbox, never in the forked child.
void AllowUserGitConfiguration(SubprocessSandbox& sandbox);

// Parent-side, read-only snapshot of whether the kernel-confinement layers used by
// ApplyChildSandbox are actually usable on this host. Because that confinement is fail-open and
// applied in a forked child right before execvp, a misconfigured or old kernel silently degrades to
// just the in-process capability gate. This probe lets the host log and report which layers are
// live. It performs ONLY version/get queries (e.g. landlock_create_ruleset version probe,
// PR_GET_SECCOMP) and never calls landlock_restrict_self or PR_SET_SECCOMP, so it does not confine
// the calling (host) process.
struct SandboxSupport {
  bool compiled_with_landlock = false;      // built with the Landlock uapi (MICROIDE_HAS_LANDLOCK)
  bool landlock_runtime_available = false;  // kernel reports a usable Landlock ABI (>= 1)
  int landlock_abi = 0;                      // reported ABI version, 0 when unavailable
  bool compiled_with_seccomp = false;       // built with the seccomp uapi (MICROIDE_HAS_SECCOMP)
  bool seccomp_runtime_available = false;   // kernel accepts the PR_GET_SECCOMP query

  // True when both kernel layers this build can install are usable on this host. When seccomp was
  // not compiled in, only the Landlock layer is required (the network block is optional anyway).
  bool fully_active() const {
    return landlock_runtime_available &&
           (compiled_with_seccomp ? seccomp_runtime_available : true);
  }
};

// Probes the running kernel once for kernel-confinement availability. Read-only and safe to call
// from the host process; see SandboxSupport. Returns an all-false snapshot on non-Linux builds.
SandboxSupport ProbeSandboxSupport();

}  // namespace microide::platform
