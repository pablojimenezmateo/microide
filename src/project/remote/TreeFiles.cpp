#include "project/remote/TreeFiles.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <utility>

#include "project/remote/RemoteManifest.h"

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace microide::project::remote {
namespace {

#if defined(__unix__) || defined(__APPLE__)

class Fd {
 public:
  Fd() = default;
  explicit Fd(int fd) : fd_(fd) {}
  ~Fd() { Reset(); }
  Fd(Fd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
  Fd& operator=(Fd&& other) noexcept {
    if (this != &other) {
      Reset();
      fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
  }
  int get() const { return fd_; }
  void Reset() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
    fd_ = -1;
  }

 private:
  int fd_ = -1;
};

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
  Fd dir;
  std::string name;
};

std::optional<Parent> OpenParent(const std::filesystem::path& root, std::string_view path,
                                 bool create, std::string* error) {
  if (!IsSafeRelativePath(path)) {
    *error = "unsafe path '" + std::string(path) + "'";
    return std::nullopt;
  }
  Fd dir(::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (dir.get() < 0) {
    *error = Errno("cannot open the workspace root " + root.string());
    return std::nullopt;
  }
  std::size_t start = 0;
  for (std::size_t slash = path.find('/'); slash != std::string_view::npos;
       start = slash + 1, slash = path.find('/', start)) {
    const std::string component(path.substr(start, slash - start));
    Fd next(::openat(dir.get(), component.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (next.get() < 0 && errno == ENOENT && create) {
      if (::mkdirat(dir.get(), component.c_str(), 0755) != 0 && errno != EEXIST) {
        *error = Errno("cannot create " + std::string(path.substr(0, slash)));
        return std::nullopt;
      }
      next = Fd(::openat(dir.get(), component.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    }
    if (next.get() < 0) {
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
  const Fd fd(::openat(dir, name.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NOCTTY | O_NONBLOCK));
  if (fd.get() < 0) {
    *error = Errno("cannot read " + name);
    return std::nullopt;
  }
  auto hash = util::HashFileDescriptor(fd.get());
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
#if defined(__linux__) && defined(RENAME_NOREPLACE)
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

#endif

}  // namespace

FileOpResult ReadTreeFile(const std::filesystem::path& root, std::string_view path,
                               std::uint64_t max_bytes,
                               const std::function<void(std::string_view chunk)>& sink) {
#if defined(__unix__) || defined(__APPLE__)
  std::string error;
  std::optional<Parent> parent = OpenParent(root, path, /*create=*/false, &error);
  if (!parent.has_value()) {
    return Error(error);
  }
  // The final component may be a link: an out-of-root link ships as its target.
  const Fd fd(::openat(parent->dir.get(), parent->name.c_str(),
                       O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK));
  if (fd.get() < 0) {
    if (errno == ENOENT) {
      return Conflict(std::nullopt);
    }
    return Error(Errno("cannot open " + std::string(path)));
  }
  struct stat info {};
  if (::fstat(fd.get(), &info) != 0 || !S_ISREG(info.st_mode)) {
    return Error(std::string(path) + " is not a regular file on the host");
  }
  if (static_cast<std::uint64_t>(info.st_size) > max_bytes) {
    return Error(std::string(path) + " is " + std::to_string(info.st_size) +
                 " bytes, over the " + std::to_string(max_bytes) + "-byte limit");
  }
  const auto hash = util::HashFileDescriptor(fd.get(), nullptr, sink);
  if (!hash.has_value()) {
    return Error(Errno("cannot read " + std::string(path)));
  }
  return Ok(hash);
#else
  (void)root;
  (void)path;
  (void)max_bytes;
  (void)sink;
  return FileOpResult{};
#endif
}

FileOpResult ReadHostFile(std::string_view path, std::uint64_t max_bytes,
                          const std::function<void(std::string_view chunk)>& sink) {
#if defined(__unix__) || defined(__APPLE__)
  const std::string text(path);
  if (text.empty() || text.front() != '/' || text.find('\0') != std::string::npos) {
    return Error("not an absolute path");
  }
  const Fd fd(::open(text.c_str(), O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK));
  if (fd.get() < 0) {
    return errno == ENOENT ? Conflict(std::nullopt) : Error(Errno("cannot open " + text));
  }
  struct stat info {};
  if (::fstat(fd.get(), &info) != 0 || !S_ISREG(info.st_mode)) {
    return Error(text + " is not a regular file on the host");
  }
  if (static_cast<std::uint64_t>(info.st_size) > max_bytes) {
    return Error(text + " is over the " + std::to_string(max_bytes) + "-byte limit");
  }
  const auto hash = util::HashFileDescriptor(fd.get(), nullptr, sink);
  if (!hash.has_value()) {
    return Error(Errno("cannot read " + text));
  }
  return Ok(hash);
#else
  (void)path;
  (void)max_bytes;
  (void)sink;
  return FileOpResult{};
#endif
}

FileOpResult WriteTreeFile(const std::filesystem::path& root, std::string_view path,
                                std::string_view content, const Precondition& expect,
                                std::optional<std::uint32_t> mode,
                                std::optional<std::int64_t> mtime_ns) {
#if defined(__unix__) || defined(__APPLE__)
  std::string error;
  std::optional<Parent> parent = OpenParent(root, path, /*create=*/true, &error);
  if (!parent.has_value()) {
    return Error(error);
  }
  FileOpResult refused;
  bool exists = false;
  if (!CheckExpect(parent->dir.get(), parent->name, expect, /*allow_non_file=*/false, &refused,
                   &exists)) {
    return refused;
  }
  mode_t final_mode = 0644;
  if (mode.has_value()) {
    final_mode = static_cast<mode_t>(*mode & 07777);
  } else if (exists) {
    struct stat info {};
    if (::fstatat(parent->dir.get(), parent->name.c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0) {
      final_mode = info.st_mode & 07777;
    }
  }
  const std::string temp = TempName(parent->name);
  {
    const Fd out(::openat(parent->dir.get(), temp.c_str(),
                          O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (out.get() < 0) {
      return Error(Errno("cannot create a temporary file beside " + std::string(path)));
    }
    std::size_t written = 0;
    while (written < content.size()) {
      const ssize_t count = ::write(out.get(), content.data() + written, content.size() - written);
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count <= 0) {
        const std::string why = Errno("cannot write " + std::string(path));
        ::unlinkat(parent->dir.get(), temp.c_str(), 0);
        return Error(why);
      }
      written += static_cast<std::size_t>(count);
    }
    if (mtime_ns.has_value()) {
      const struct timespec times[2] = {
          {0, UTIME_OMIT},
          {static_cast<time_t>(*mtime_ns / 1'000'000'000), static_cast<long>(*mtime_ns % 1'000'000'000)},
      };
      (void)::futimens(out.get(), times);  // best effort: only a fast path depends on it
    }
    if (::fchmod(out.get(), final_mode) != 0 || ::fsync(out.get()) != 0) {
      const std::string why = Errno("cannot finish " + std::string(path));
      ::unlinkat(parent->dir.get(), temp.c_str(), 0);
      return Error(why);
    }
  }
  const int renamed =
      expect.kind == Precondition::Kind::Absent
          ? RenameNoReplace(parent->dir.get(), temp.c_str(), parent->dir.get(), parent->name.c_str())
          : ::renameat(parent->dir.get(), temp.c_str(), parent->dir.get(), parent->name.c_str());
  if (renamed != 0) {
    const int saved = errno;
    ::unlinkat(parent->dir.get(), temp.c_str(), 0);
    if (saved == EEXIST) {
      // Created on the host between the check and the rename.
      bool now_exists = false;
      bool not_a_file = false;
      std::string ignored;
      return Conflict(CurrentHash(parent->dir.get(), parent->name, &now_exists, &not_a_file, &ignored));
    }
    errno = saved;
    return Error(Errno("cannot replace " + std::string(path)));
  }
  return Ok(util::HashContent(content));
#else
  (void)root;
  (void)path;
  (void)content;
  (void)expect;
  (void)mode;
  (void)mtime_ns;
  return FileOpResult{};
#endif
}

FileOpResult MakeTreeSymlink(const std::filesystem::path& root, std::string_view path,
                             std::string_view target) {
#if defined(__unix__) || defined(__APPLE__)
  std::string error;
  std::optional<Parent> parent = OpenParent(root, path, /*create=*/true, &error);
  if (!parent.has_value()) {
    return Error(error);
  }
  struct stat info {};
  if (::fstatat(parent->dir.get(), parent->name.c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0 &&
      !S_ISLNK(info.st_mode)) {
    return Conflict(std::nullopt);
  }
  const std::string temp = TempName(parent->name);
  const std::string target_text(target);
  if (::symlinkat(target_text.c_str(), parent->dir.get(), temp.c_str()) != 0) {
    return Error(Errno("cannot create the link " + std::string(path)));
  }
  if (::renameat(parent->dir.get(), temp.c_str(), parent->dir.get(), parent->name.c_str()) != 0) {
    const std::string why = Errno("cannot place the link " + std::string(path));
    ::unlinkat(parent->dir.get(), temp.c_str(), 0);
    return Error(why);
  }
  return Ok(std::nullopt);
#else
  (void)root;
  (void)path;
  (void)target;
  return FileOpResult{};
#endif
}

FileOpResult MakeTreeDirectory(const std::filesystem::path& root, std::string_view path) {
#if defined(__unix__) || defined(__APPLE__)
  std::string error;
  std::optional<Parent> parent = OpenParent(root, path, /*create=*/true, &error);
  if (!parent.has_value()) {
    return Error(error);
  }
  if (::mkdirat(parent->dir.get(), parent->name.c_str(), 0755) != 0) {
    struct stat info {};
    if (errno == EEXIST &&
        ::fstatat(parent->dir.get(), parent->name.c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0 &&
        S_ISDIR(info.st_mode)) {
      return Ok(std::nullopt);
    }
    return Error(Errno("cannot create " + std::string(path)));
  }
  return Ok(std::nullopt);
#else
  (void)root;
  (void)path;
  return FileOpResult{};
#endif
}

FileOpResult RenameTreeEntry(const std::filesystem::path& root, std::string_view from,
                                  std::string_view to, const Precondition& expect) {
#if defined(__unix__) || defined(__APPLE__)
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
  if (!CheckExpect(source->dir.get(), source->name, expect, /*allow_non_file=*/true, &refused,
                   &exists)) {
    return refused;
  }
  if (!exists) {
    return Conflict(std::nullopt);
  }
  if (RenameNoReplace(source->dir.get(), source->name.c_str(), target->dir.get(),
                      target->name.c_str()) != 0) {
    if (errno == EEXIST) {
      bool now_exists = false;
      bool not_a_file = false;
      std::string ignored;
      return Conflict(CurrentHash(target->dir.get(), target->name, &now_exists, &not_a_file, &ignored));
    }
    return Error(Errno("cannot rename " + std::string(from) + " to " + std::string(to)));
  }
  return Ok(std::nullopt);
#else
  (void)root;
  (void)from;
  (void)to;
  (void)expect;
  return FileOpResult{};
#endif
}

FileOpResult DeleteTreeEntry(const std::filesystem::path& root, std::string_view path,
                                  const Precondition& expect) {
#if defined(__unix__) || defined(__APPLE__)
  std::string error;
  std::optional<Parent> parent = OpenParent(root, path, /*create=*/false, &error);
  if (!parent.has_value()) {
    return Error(error);
  }
  FileOpResult refused;
  bool exists = false;
  if (!CheckExpect(parent->dir.get(), parent->name, expect, /*allow_non_file=*/true, &refused,
                   &exists)) {
    return refused;
  }
  if (!exists) {
    return Ok(std::nullopt);  // already gone: what the caller wanted
  }
  struct stat info {};
  const bool directory =
      ::fstatat(parent->dir.get(), parent->name.c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0 &&
      S_ISDIR(info.st_mode);
  if (::unlinkat(parent->dir.get(), parent->name.c_str(), directory ? AT_REMOVEDIR : 0) != 0) {
    return Error(Errno("cannot delete " + std::string(path)));
  }
  return Ok(std::nullopt);
#else
  (void)root;
  (void)path;
  (void)expect;
  return FileOpResult{};
#endif
}

}  // namespace microide::project::remote
