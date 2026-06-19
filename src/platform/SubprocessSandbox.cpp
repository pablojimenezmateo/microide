#include "platform/SubprocessSandbox.h"

#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#endif

#if defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#endif

// Landlock (filesystem) — guarded so the build still works on kernels/toolchains without the uapi.
#if defined(__linux__) && defined(__has_include)
#if __has_include(<linux/landlock.h>)
#define MICROIDE_HAS_LANDLOCK 1
#include <linux/landlock.h>
#include <sys/syscall.h>
#endif
#endif

// seccomp (network) — only meaningful on architectures whose audit arch + syscall numbers we know.
#if defined(__linux__) && defined(__has_include) && (defined(__x86_64__) || defined(__aarch64__))
#if __has_include(<linux/seccomp.h>) && __has_include(<linux/filter.h>) && \
    __has_include(<linux/audit.h>)
#define MICROIDE_HAS_SECCOMP 1
#include <errno.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#endif
#endif

namespace microide::platform {
namespace {

#if defined(__unix__) || defined(__APPLE__)
void ApplyLimit(int resource, long value) {
  if (value <= 0) {
    return;
  }
  rlimit limit{};
  limit.rlim_cur = static_cast<rlim_t>(value);
  limit.rlim_max = static_cast<rlim_t>(value);
  setrlimit(resource, &limit);  // Best-effort; ignore failure.
}

void ApplyResourceLimits(const SubprocessResourceLimits& limits) {
  ApplyLimit(RLIMIT_CPU, limits.cpu_seconds);
  ApplyLimit(RLIMIT_AS, limits.address_space_bytes);
  ApplyLimit(RLIMIT_NOFILE, limits.open_files);
  ApplyLimit(RLIMIT_FSIZE, limits.file_size_bytes);
}
#endif

#if defined(MICROIDE_HAS_LANDLOCK)

int LandlockCreateRuleset(const landlock_ruleset_attr* attr, size_t size, __u32 flags) {
  return static_cast<int>(syscall(__NR_landlock_create_ruleset, attr, size, flags));
}
int LandlockAddRule(int ruleset_fd, landlock_rule_type type, const void* attr, __u32 flags) {
  return static_cast<int>(syscall(__NR_landlock_add_rule, ruleset_fd, type, attr, flags));
}
int LandlockRestrictSelf(int ruleset_fd, __u32 flags) {
  return static_cast<int>(syscall(__NR_landlock_restrict_self, ruleset_fd, flags));
}

// Read/execute access bits that exist since Landlock ABI v1. Reading and executing the system is
// always permitted; this is what keeps execvp and shared-library loading working under the ruleset.
__u64 ReadAccess() {
  return LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR | LANDLOCK_ACCESS_FS_EXECUTE;
}

// Full read+write+manage access, down-masked to what the running kernel's ABI actually supports so
// landlock_add_rule does not reject unknown bits. Newer access types are added only when both the
// header defines them and the detected ABI is recent enough.
__u64 WriteAccess(int abi) {
  __u64 access = ReadAccess() | LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_REMOVE_DIR |
                 LANDLOCK_ACCESS_FS_REMOVE_FILE | LANDLOCK_ACCESS_FS_MAKE_CHAR |
                 LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_MAKE_REG |
                 LANDLOCK_ACCESS_FS_MAKE_SOCK | LANDLOCK_ACCESS_FS_MAKE_FIFO |
                 LANDLOCK_ACCESS_FS_MAKE_BLOCK | LANDLOCK_ACCESS_FS_MAKE_SYM;
#if defined(LANDLOCK_ACCESS_FS_REFER)
  if (abi >= 2) {
    access |= LANDLOCK_ACCESS_FS_REFER;
  }
#endif
#if defined(LANDLOCK_ACCESS_FS_TRUNCATE)
  if (abi >= 3) {
    access |= LANDLOCK_ACCESS_FS_TRUNCATE;
  }
#endif
  return access;
}

bool AddRule(int ruleset_fd, const char* path, __u64 access) {
  const int path_fd = open(path, O_PATH | O_CLOEXEC);
  if (path_fd < 0) {
    return false;  // Path absent on this system; nothing to grant.
  }
  landlock_path_beneath_attr attr{};
  attr.allowed_access = access;
  attr.parent_fd = path_fd;
  const int rc = LandlockAddRule(ruleset_fd, LANDLOCK_RULE_PATH_BENEATH, &attr, 0);
  close(path_fd);
  return rc == 0;
}

// Confines the child's filesystem writes to `write_roots` while leaving the wider system readable
// and executable. The handled-access set covers read+write; any path not granted a given access is
// denied it. Returns without restricting (fail-open) when the kernel lacks Landlock support.
void ApplyLandlock(const std::vector<std::filesystem::path>& read_roots,
                   const std::vector<std::filesystem::path>& write_roots) {
  const int abi = LandlockCreateRuleset(nullptr, 0, LANDLOCK_CREATE_RULESET_VERSION);
  if (abi < 1) {
    return;  // Landlock unavailable or disabled.
  }
  const __u64 read_access = ReadAccess();
  const __u64 write_access = WriteAccess(abi);

  landlock_ruleset_attr ruleset{};
  ruleset.handled_access_fs = write_access;  // Superset of read_access.
  const int ruleset_fd = LandlockCreateRuleset(&ruleset, sizeof(ruleset), 0);
  if (ruleset_fd < 0) {
    return;
  }

  // System roots stay read+execute so binaries, the loader, and shared libraries resolve. /tmp,
  // /dev, and /run keep read+write because most tools need scratch space and /dev/null.
  static const char* const kSystemReadRoots[] = {"/usr", "/bin",  "/sbin", "/lib",
                                                  "/lib64", "/etc", "/opt",  "/proc",
                                                  "/sys"};
  static const char* const kSystemWriteRoots[] = {"/tmp", "/dev", "/run", "/var/tmp"};
  for (const char* root : kSystemReadRoots) {
    AddRule(ruleset_fd, root, read_access);
  }
  for (const char* root : kSystemWriteRoots) {
    AddRule(ruleset_fd, root, write_access);
  }
  for (const std::filesystem::path& root : read_roots) {
    if (!root.empty()) {
      AddRule(ruleset_fd, root.c_str(), read_access);
    }
  }
  for (const std::filesystem::path& root : write_roots) {
    if (!root.empty()) {
      AddRule(ruleset_fd, root.c_str(), write_access);
    }
  }

  if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0) {
    LandlockRestrictSelf(ruleset_fd, 0);
  }
  close(ruleset_fd);
}
#endif  // MICROIDE_HAS_LANDLOCK

#if defined(MICROIDE_HAS_SECCOMP)

#if defined(__x86_64__)
constexpr __u32 kAuditArch = AUDIT_ARCH_X86_64;
#elif defined(__aarch64__)
constexpr __u32 kAuditArch = AUDIT_ARCH_AARCH64;
#endif

// Installs a seccomp filter that fails socket(AF_INET/AF_INET6, ...) with EACCES while allowing
// every other syscall (including AF_UNIX/AF_NETLINK sockets, so local IPC keeps working). Loading
// args[0] as a 32-bit word reads the socket domain on the little-endian arches we gate on.
void ApplyNetworkBlock() {
  sock_filter filter[] = {
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, arch)),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, kAuditArch, 1, 0),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),  // Foreign arch (e.g. 32-bit compat): allow.
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_socket, 0, 4),
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, args[0])),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AF_INET, 1, 0),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AF_INET6, 0, 1),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EACCES & SECCOMP_RET_DATA)),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
  };
  sock_fprog program{};
  program.len = static_cast<unsigned short>(sizeof(filter) / sizeof(filter[0]));
  program.filter = filter;

  if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
    return;
  }
  prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program, 0, 0);  // Best-effort.
}
#endif  // MICROIDE_HAS_SECCOMP

}  // namespace

void ApplyChildSandbox(const SubprocessSandbox& sandbox) {
  // Resource ceilings are independent of the filesystem/network confinement: a
  // memory/CPU cap is a safety limit we want even when the child is otherwise
  // unconfined (e.g. a debug adapter, which must keep writing its debuginfo index
  // cache to ~/.cache and so cannot run under the Landlock write ruleset). Limits
  // default to 0 (= inherit the parent's), so this is a no-op unless a caller set
  // one.
#if defined(__unix__) || defined(__APPLE__)
  ApplyResourceLimits(sandbox.limits);
#endif
  if (!sandbox.enabled) {
    return;
  }
#if defined(MICROIDE_HAS_LANDLOCK)
  ApplyLandlock(sandbox.read_roots, sandbox.write_roots);
#endif
#if defined(MICROIDE_HAS_SECCOMP)
  if (!sandbox.allow_network) {
    ApplyNetworkBlock();
  }
#endif
  (void)sandbox;
}

}  // namespace microide::platform
