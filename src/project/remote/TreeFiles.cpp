#include "project/remote/TreeFiles.h"

#include "util/PerformanceCounters.h"
#include "util/PosixFd.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <utility>

#include "project/remote/RemoteManifest.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace microide::project::remote {
namespace {

using util::UniqueFd;

FileOpResult Error(std::string message) {
  FileOpResult result;
  result.status = FileOpResult::Status::Error;
  result.error = std::move(message);
  return result;
}

FileOpResult Conflict(std::optional<util::ContentHash> current) {
  FileOpResult result;
  result.status = FileOpResult::Status::Conflict;
  result.current = current;
  return result;
}

FileOpResult Ok(std::optional<util::ContentHash> current) {
  FileOpResult result;
  result.status = FileOpResult::Status::Ok;
  result.current = current;
  return result;
}

std::string Errno(std::string_view what) {
  return std::string(what) + ": " + std::strerror(errno);
}

// The directory holding `path`'s last component, reached one component at a time
// with O_NOFOLLOW (a link in a parent position fails with ELOOP/ENOTDIR instead of
// being followed), and that last component's name.
struct Parent {
  UniqueFd dir;
  std::string name;
};

std::optional<Parent> OpenParent(const std::filesystem::path& root, std::string_view path,
                                 bool create, std::string* error) {
  if (!IsSafeRelativePath(path)) {
    *error = "unsafe path '" + std::string(path) + "'";
    return std::nullopt;
  }
  UniqueFd dir(::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (dir.Get() < 0) {
    *error = Errno("cannot open the workspace root " + root.string());
    return std::nullopt;
  }
  std::size_t start = 0;
  for (std::size_t slash = path.find('/'); slash != std::string_view::npos;
       start = slash + 1, slash = path.find('/', start)) {
    const std::string component(path.substr(start, slash - start));
    UniqueFd next(::openat(dir.Get(), component.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (next.Get() < 0 && errno == ENOENT && create) {
      if (::mkdirat(dir.Get(), component.c_str(), 0755) != 0 && errno != EEXIST) {
        *error = Errno("cannot create " + std::string(path.substr(0, slash)));
        return std::nullopt;
      }
      next = UniqueFd(::openat(dir.Get(), component.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    }
    if (next.Get() < 0) {
      *error = errno == ELOOP || errno == ENOTDIR
                   ? std::string(path.substr(0, slash)) + " is not a directory on the host"
                   : Errno("cannot open " + std::string(path.substr(0, slash)));
      return std::nullopt;
    }
    dir = std::move(next);
  }
  return Parent{std::move(dir), std::string(path.substr(start))};
}

// What `name` in `dir` holds: nullopt when nothing is there. A link, a directory or
// anything but a regular file sets *not_a_file (its content has no hash).
std::optional<util::ContentHash> CurrentHash(int dir, const std::string& name, bool* exists,
                                             bool* not_a_file, std::string* error) {
  *exists = false;
  *not_a_file = false;
  struct stat info {};
  if (::fstatat(dir, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) {
    if (errno != ENOENT) {
      *error = Errno("cannot stat " + name);
    }
    return std::nullopt;
  }
  *exists = true;
  if (!S_ISREG(info.st_mode)) {
    *not_a_file = true;
    return std::nullopt;
  }
  const UniqueFd fd(::openat(dir, name.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NOCTTY | O_NONBLOCK));
  if (fd.Get() < 0) {
    *error = Errno("cannot read " + name);
    return std::nullopt;
  }
  auto hash = util::HashFileDescriptor(fd.Get());
  if (!hash.has_value()) {
    *error = Errno("cannot read " + name);
  }
  return hash;
}

// Whether `expect` holds for `name`; on false, `*result` is the answer to send.
bool CheckExpect(int dir, const std::string& name, const Precondition& expect, bool allow_non_file,
                 FileOpResult* result, bool* exists) {
  if (expect.kind == Precondition::Kind::Any && allow_non_file) {
    struct stat info {};
    *exists = ::fstatat(dir, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0;
    return true;
  }
  bool not_a_file = false;
  std::string error;
  const auto current = CurrentHash(dir, name, exists, &not_a_file, &error);
  if (!error.empty()) {
    *result = Error(error);
    return false;
  }
  if (not_a_file && !allow_non_file) {
    *result = Error(name + " is not a regular file on the host");
    return false;
  }
  switch (expect.kind) {
    case Precondition::Kind::Any:
      return true;
    case Precondition::Kind::Absent:
      if (*exists) {
        *result = Conflict(current);
        return false;
      }
      return true;
    case Precondition::Kind::Hash:
      if (!current.has_value() || *current != expect.hash) {
        *result = Conflict(current);
        return false;
      }
      return true;
  }
  return false;
}

// renameat2(RENAME_NOREPLACE), falling back to link+unlink where the filesystem
// does not support it (link refuses an existing destination just the same).
int RenameNoReplace(int from_dir, const char* from, int to_dir, const char* to) {
#if defined(RENAME_NOREPLACE)
  if (::renameat2(from_dir, from, to_dir, to, RENAME_NOREPLACE) == 0) {
    return 0;
  }
  if (errno != EINVAL && errno != ENOSYS) {
    return -1;
  }
#endif
  if (::linkat(from_dir, from, to_dir, to, 0) != 0) {
    return -1;
  }
  ::unlinkat(from_dir, from, 0);
  return 0;
}

std::string TempName(const std::string& name) {
  static std::atomic<std::uint64_t> counter{0};
  return "." + name.substr(0, 200) + ".microide-" + std::to_string(::getpid()) + "-" +
         std::to_string(counter.fetch_add(1)) + ".tmp";
}

}  // namespace

FileOpResult ReadTreeFile(const std::filesystem::path& root, std::string_view path,
                               std::uint64_t max_bytes,
                               const std::function<void(std::string_view chunk)>& sink) {
  std::string error;
  std::optional<Parent> parent = OpenParent(root, path, /*create=*/false, &error);
  if (!parent.has_value()) {
    return Error(error);
  }
  // The final component may be a link: an out-of-root link ships as its target.
  const UniqueFd fd(::openat(parent->dir.Get(), parent->name.c_str(),
                       O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK));
  if (fd.Get() < 0) {
    if (errno == ENOENT) {
      return Conflict(std::nullopt);
    }
    return Error(Errno("cannot open " + std::string(path)));
  }
  struct stat info {};
  if (::fstat(fd.Get(), &info) != 0 || !S_ISREG(info.st_mode)) {
    return Error(std::string(path) + " is not a regular file on the host");
  }
  if (static_cast<std::uint64_t>(info.st_size) > max_bytes) {
    return Error(std::string(path) + " is " + std::to_string(info.st_size) +
                 " bytes, over the " + std::to_string(max_bytes) + "-byte limit");
  }
  const auto hash = util::HashFileDescriptor(fd.Get(), nullptr, sink);
  if (!hash.has_value()) {
    return Error(Errno("cannot read " + std::string(path)));
  }
  return Ok(hash);
}

FileOpResult ReadHostFile(std::string_view path, std::uint64_t max_bytes,
                          const std::function<void(std::string_view chunk)>& sink) {
  const std::string text(path);
  if (text.empty() || text.front() != '/' || text.find('\0') != std::string::npos) {
    return Error("not an absolute path");
  }
  const UniqueFd fd(::open(text.c_str(), O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK));
  if (fd.Get() < 0) {
    return errno == ENOENT ? Conflict(std::nullopt) : Error(Errno("cannot open " + text));
  }
  struct stat info {};
  if (::fstat(fd.Get(), &info) != 0 || !S_ISREG(info.st_mode)) {
    return Error(text + " is not a regular file on the host");
  }
  if (static_cast<std::uint64_t>(info.st_size) > max_bytes) {
    return Error(text + " is over the " + std::to_string(max_bytes) + "-byte limit");
  }
  const auto hash = util::HashFileDescriptor(fd.Get(), nullptr, sink);
  if (!hash.has_value()) {
    return Error(Errno("cannot read " + text));
  }
  return Ok(hash);
}

namespace {

// WriteTreeFile's body, with the bytes supplied by `fill`: it writes them to the
// temp file's descriptor and returns their hash (nullopt with *error on failure).
FileOpResult WriteTreeFileFrom(const std::filesystem::path& root, std::string_view path,
                               const std::function<std::optional<util::ContentHash>(int fd, std::string* error)>& fill,
                               const Precondition& expect, std::optional<std::uint32_t> mode,
                               std::optional<std::int64_t> mtime_ns, Durability durability) {
  std::string error;
  std::optional<Parent> parent = OpenParent(root, path, /*create=*/true, &error);
  if (!parent.has_value()) {
    return Error(error);
  }
  FileOpResult refused;
  bool exists = false;
  if (!CheckExpect(parent->dir.Get(), parent->name, expect, /*allow_non_file=*/false, &refused,
                   &exists)) {
    return refused;
  }
  mode_t final_mode = 0644;
  if (mode.has_value()) {
    final_mode = static_cast<mode_t>(*mode & 07777);
  } else if (exists) {
    struct stat info {};
    if (::fstatat(parent->dir.Get(), parent->name.c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0) {
      final_mode = info.st_mode & 07777;
    }
  }
  const std::string temp = TempName(parent->name);
  std::optional<util::ContentHash> hash;
  {
    const UniqueFd out(::openat(parent->dir.Get(), temp.c_str(),
                          O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (out.Get() < 0) {
      return Error(Errno("cannot create a temporary file beside " + std::string(path)));
    }
    std::string fill_error;
    hash = fill(out.Get(), &fill_error);
    if (!hash.has_value()) {
      ::unlinkat(parent->dir.Get(), temp.c_str(), 0);
      return Error(fill_error.empty() ? Errno("cannot write " + std::string(path)) : fill_error);
    }
    if (mtime_ns.has_value()) {
      const struct timespec times[2] = {
          {0, UTIME_OMIT},
          {static_cast<time_t>(*mtime_ns / 1'000'000'000), static_cast<long>(*mtime_ns % 1'000'000'000)},
      };
      (void)::futimens(out.Get(), times);  // best effort: only a fast path depends on it
    }
    if (durability == Durability::Synced) {
      util::AddPerformanceCounter(util::PerfCounterId::RemoteTreeFileFsyncs);
    }
    if (::fchmod(out.Get(), final_mode) != 0 ||
        (durability == Durability::Synced && ::fsync(out.Get()) != 0)) {
      const std::string why = Errno("cannot finish " + std::string(path));
      ::unlinkat(parent->dir.Get(), temp.c_str(), 0);
      return Error(why);
    }
  }
  const int renamed =
      expect.kind == Precondition::Kind::Absent
          ? RenameNoReplace(parent->dir.Get(), temp.c_str(), parent->dir.Get(), parent->name.c_str())
          : ::renameat(parent->dir.Get(), temp.c_str(), parent->dir.Get(), parent->name.c_str());
  if (renamed != 0) {
    const int saved = errno;
    ::unlinkat(parent->dir.Get(), temp.c_str(), 0);
    if (saved == EEXIST) {
      // Created on the host between the check and the rename.
      bool now_exists = false;
      bool not_a_file = false;
      std::string ignored;
      return Conflict(CurrentHash(parent->dir.Get(), parent->name, &now_exists, &not_a_file, &ignored));
    }
    errno = saved;
    return Error(Errno("cannot replace " + std::string(path)));
  }
  return Ok(*hash);
}

}  // namespace

bool SyncTreeFilesystem(const std::filesystem::path& root) {
  util::AddPerformanceCounter(util::PerfCounterId::RemoteTreeFlushes);
  const UniqueFd dir(::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  return dir.Get() >= 0 && ::syncfs(dir.Get()) == 0;
}

FileOpResult WriteTreeFile(const std::filesystem::path& root, std::string_view path,
                           std::string_view content, const Precondition& expect,
                           std::optional<std::uint32_t> mode, std::optional<std::int64_t> mtime_ns,
                           Durability durability) {
  return WriteTreeFileFrom(
      root, path,
      [&](int fd, std::string* error) -> std::optional<util::ContentHash> {
        std::size_t written = 0;
        while (written < content.size()) {
          const ssize_t count = ::write(fd, content.data() + written, content.size() - written);
          if (count < 0 && errno == EINTR) {
            continue;
          }
          if (count <= 0) {
            *error = Errno("cannot write " + std::string(path));
            return std::nullopt;
          }
          written += static_cast<std::size_t>(count);
        }
        return util::HashContent(content);
      },
      expect, mode, mtime_ns, durability);
}

FileOpResult CopyFileIntoTree(const std::filesystem::path& root, std::string_view path,
                              const std::filesystem::path& source, const util::ContentHash& hash,
                              const Precondition& expect, std::optional<std::uint32_t> mode,
                              std::optional<std::int64_t> mtime_ns, Durability durability) {
  return WriteTreeFileFrom(
      root, path,
      [&](int fd, std::string* error) -> std::optional<util::ContentHash> {
        const UniqueFd in(::open(source.c_str(), O_RDONLY | O_CLOEXEC));
        if (in.Get() < 0) {
          *error = Errno("cannot open the fetched copy of " + std::string(path));
          return std::nullopt;
        }
        // In the kernel, and a reflink where the filesystem can: no bytes through here.
        for (;;) {
          const ssize_t count = ::copy_file_range(in.Get(), nullptr, fd, nullptr, 1u << 30, 0);
          if (count == 0) {
            return hash;
          }
          if (count < 0) {
            if (errno == EINTR) {
              continue;
            }
            if (errno == EXDEV || errno == ENOSYS || errno == EINVAL || errno == EOPNOTSUPP) {
              break;  // not supported here: copy by hand below, from where it got to
            }
            *error = Errno("cannot copy " + std::string(path));
            return std::nullopt;
          }
        }
        char buffer[64 * 1024];
        for (;;) {
          const ssize_t count = ::read(in.Get(), buffer, sizeof(buffer));
          if (count < 0 && errno == EINTR) {
            continue;
          }
          if (count < 0) {
            *error = Errno("cannot read the fetched copy of " + std::string(path));
            return std::nullopt;
          }
          if (count == 0) {
            return hash;
          }
          for (ssize_t done = 0; done < count;) {
            const ssize_t wrote = ::write(fd, buffer + done, static_cast<std::size_t>(count - done));
            if (wrote < 0 && errno == EINTR) {
              continue;
            }
            if (wrote <= 0) {
              *error = Errno("cannot write " + std::string(path));
              return std::nullopt;
            }
            done += wrote;
          }
        }
      },
      expect, mode, mtime_ns, durability);
}

FileOpResult MakeTreeSymlink(const std::filesystem::path& root, std::string_view path,
                             std::string_view target) {
  std::string error;
  std::optional<Parent> parent = OpenParent(root, path, /*create=*/true, &error);
  if (!parent.has_value()) {
    return Error(error);
  }
  struct stat info {};
  if (::fstatat(parent->dir.Get(), parent->name.c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0 &&
      !S_ISLNK(info.st_mode)) {
    return Conflict(std::nullopt);
  }
  const std::string temp = TempName(parent->name);
  const std::string target_text(target);
  if (::symlinkat(target_text.c_str(), parent->dir.Get(), temp.c_str()) != 0) {
    return Error(Errno("cannot create the link " + std::string(path)));
  }
  if (::renameat(parent->dir.Get(), temp.c_str(), parent->dir.Get(), parent->name.c_str()) != 0) {
    const std::string why = Errno("cannot place the link " + std::string(path));
    ::unlinkat(parent->dir.Get(), temp.c_str(), 0);
    return Error(why);
  }
  return Ok(std::nullopt);
}

FileOpResult MakeTreeDirectory(const std::filesystem::path& root, std::string_view path) {
  std::string error;
  std::optional<Parent> parent = OpenParent(root, path, /*create=*/true, &error);
  if (!parent.has_value()) {
    return Error(error);
  }
  if (::mkdirat(parent->dir.Get(), parent->name.c_str(), 0755) != 0) {
    struct stat info {};
    if (errno == EEXIST &&
        ::fstatat(parent->dir.Get(), parent->name.c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0 &&
        S_ISDIR(info.st_mode)) {
      return Ok(std::nullopt);
    }
    return Error(Errno("cannot create " + std::string(path)));
  }
  return Ok(std::nullopt);
}

FileOpResult RenameTreeEntry(const std::filesystem::path& root, std::string_view from,
                                  std::string_view to, const Precondition& expect) {
  std::string error;
  std::optional<Parent> source = OpenParent(root, from, /*create=*/false, &error);
  if (!source.has_value()) {
    return Error(error);
  }
  std::optional<Parent> target = OpenParent(root, to, /*create=*/true, &error);
  if (!target.has_value()) {
    return Error(error);
  }
  FileOpResult refused;
  bool exists = false;
  if (!CheckExpect(source->dir.Get(), source->name, expect, /*allow_non_file=*/true, &refused,
                   &exists)) {
    return refused;
  }
  if (!exists) {
    return Conflict(std::nullopt);
  }
  if (RenameNoReplace(source->dir.Get(), source->name.c_str(), target->dir.Get(),
                      target->name.c_str()) != 0) {
    if (errno == EEXIST) {
      bool now_exists = false;
      bool not_a_file = false;
      std::string ignored;
      return Conflict(CurrentHash(target->dir.Get(), target->name, &now_exists, &not_a_file, &ignored));
    }
    return Error(Errno("cannot rename " + std::string(from) + " to " + std::string(to)));
  }
  return Ok(std::nullopt);
}

FileOpResult DeleteTreeEntry(const std::filesystem::path& root, std::string_view path,
                                  const Precondition& expect) {
  std::string error;
  std::optional<Parent> parent = OpenParent(root, path, /*create=*/false, &error);
  if (!parent.has_value()) {
    return Error(error);
  }
  FileOpResult refused;
  bool exists = false;
  if (!CheckExpect(parent->dir.Get(), parent->name, expect, /*allow_non_file=*/true, &refused,
                   &exists)) {
    return refused;
  }
  if (!exists) {
    return Ok(std::nullopt);  // already gone: what the caller wanted
  }
  struct stat info {};
  const bool directory =
      ::fstatat(parent->dir.Get(), parent->name.c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0 &&
      S_ISDIR(info.st_mode);
  if (::unlinkat(parent->dir.Get(), parent->name.c_str(), directory ? AT_REMOVEDIR : 0) != 0) {
    return Error(Errno("cannot delete " + std::string(path)));
  }
  return Ok(std::nullopt);
}

}  // namespace microide::project::remote
