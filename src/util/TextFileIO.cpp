#include "util/TextFileIO.h"

#include <atomic>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <system_error>

#include "util/DurableFile.h"

namespace microide::util {

namespace {

// Non-throwing check that `path` resolves (following symlinks) to a regular file.
// Opening a FIFO, device node, or procfs/sysfs entry with std::ifstream can block
// on open or seek in implementation-specific ways *before* any later size guard
// runs, so every text-read entry point rejects non-regular paths up front. A
// directory, socket, dangling symlink, or stat error all report false here.
bool IsRegularFileFollowingSymlinks(const std::filesystem::path& path) {
  std::error_code error;
  const std::filesystem::file_status status = std::filesystem::status(path, error);
  if (error) {
    return false;
  }
  return std::filesystem::is_regular_file(status);
}

std::atomic<std::size_t> g_text_search_read_count{0};

}  // namespace

std::optional<std::string> ReadTextFile(const std::filesystem::path& path) {
  // Prove the target is a regular file before opening: a special file (FIFO,
  // device, procfs entry) can block the caller on open/seek before the size
  // guard below can reject it.
  if (!IsRegularFileFollowingSymlinks(path)) {
    return std::nullopt;
  }

  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return std::nullopt;
  }

  file.seekg(0, std::ios::end);
  const std::streamoff size = file.tellg();
  if (size < 0) {
    return std::nullopt;
  }
  // Refuse pathologically large files before allocating: a multi-GB or sparse
  // file would otherwise force a single huge allocation and an uncaught bad_alloc.
  if (static_cast<std::uintmax_t>(size) > kMaxTextFileBytes) {
    return std::nullopt;
  }
  file.seekg(0, std::ios::beg);

  std::string content(static_cast<std::size_t>(size), '\0');
  if (!content.empty()) {
    file.read(content.data(), static_cast<std::streamsize>(content.size()));
    if (!file) {
      return std::nullopt;
    }
  }
  return content;
}

std::vector<std::string> ReadFileLineWindow(const std::filesystem::path& path,
                                            std::size_t first_line, std::size_t last_line,
                                            std::uintmax_t max_bytes) {
  std::vector<std::string> window;
  if (first_line == 0) {
    first_line = 1;
  }
  if (last_line < first_line) {
    return window;
  }
  // Reject non-regular files before opening: a FIFO/device/procfs entry can block
  // the caller on open/read before any byte budget applies.
  if (!IsRegularFileFollowingSymlinks(path)) {
    return window;
  }
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return window;
  }

  window.reserve(last_line - first_line + 1);
  std::string current;          // bytes of the line currently being accumulated
  std::size_t line_number = 1;  // 1-based number of the line `current` belongs to
  std::uintmax_t consumed = 0;  // total bytes streamed (bounds time + allocation)
  bool binary = false;
  bool budget_hit = false;
  char chunk[64 * 1024];

  const auto flush_line = [&]() {
    if (line_number >= first_line && line_number <= last_line) {
      if (!current.empty() && current.back() == '\r') {
        current.pop_back();
      }
      window.push_back(current);
    }
    current.clear();
  };

  while (file && !binary && !budget_hit && line_number <= last_line) {
    file.read(chunk, sizeof(chunk));
    const std::streamsize got = file.gcount();
    for (std::streamsize i = 0; i < got; ++i) {
      if (++consumed > max_bytes) {
        budget_hit = true;
        break;
      }
      const char c = chunk[i];
      if (c == '\0') {
        binary = true;
        break;
      }
      if (c == '\n') {
        flush_line();
        if (line_number >= last_line) {
          break;
        }
        ++line_number;
      } else if (line_number >= first_line && line_number <= last_line) {
        // Retain bytes only for lines we may emit; skipped lines still advance
        // line_number via their newline but cost no allocation.
        current.push_back(c);
      }
    }
    if (got < static_cast<std::streamsize>(sizeof(chunk))) {
      break;  // short read == EOF
    }
  }

  // Emit a final line that ended at EOF without a trailing newline, but only when
  // the scan stopped cleanly (not on the binary or byte-budget guard, which leave
  // `current` holding an incomplete/oversized line we must not surface).
  if (!binary && !budget_hit && !current.empty() && line_number >= first_line &&
      line_number <= last_line) {
    flush_line();
  }
  if (binary) {
    window.clear();
  }
  return window;
}

TextFileReadResult ReadTextFileClassified(const std::filesystem::path& path) {
  TextFileReadResult result;

  // Treat a genuinely-absent file as Missing (empty is a valid "deleted" state).
  // Any stat error other than not-found is conservatively an Unreadable error.
  std::error_code exist_error;
  const bool exists = std::filesystem::exists(path, exist_error);
  if (exist_error) {
    result.status = TextFileReadStatus::Unreadable;
    return result;
  }
  if (!exists) {
    result.status = TextFileReadStatus::Missing;
    return result;
  }

  // A path that exists but is not a regular file (directory, FIFO, device,
  // socket, procfs entry) must be rejected before opening: ifstream open/seek on
  // a special file can block instead of failing cleanly. Classify as Unreadable
  // rather than opening it to find out.
  if (!IsRegularFileFollowingSymlinks(path)) {
    result.status = TextFileReadStatus::Unreadable;
    return result;
  }

  std::ifstream file(path, std::ios::binary);
  if (!file) {
    // Exists (checked above) but cannot be opened -> permission/I-O error, not absence.
    result.status = TextFileReadStatus::Unreadable;
    return result;
  }

  file.seekg(0, std::ios::end);
  const std::streamoff size = file.tellg();
  if (size < 0) {
    result.status = TextFileReadStatus::Unreadable;
    return result;
  }
  if (static_cast<std::uintmax_t>(size) > kMaxTextFileBytes) {
    result.status = TextFileReadStatus::TooLarge;
    return result;
  }
  file.seekg(0, std::ios::beg);

  result.content.resize(static_cast<std::size_t>(size));
  if (size > 0) {
    file.read(result.content.data(), static_cast<std::streamsize>(size));
    if (!file) {
      result.content.clear();
      result.status = TextFileReadStatus::Unreadable;
      return result;
    }
    // An embedded NUL means the bytes are binary; refuse to treat them as text so a
    // binary file is never diffed line-by-line or saved back through a compare tab.
    if (std::memchr(result.content.data(), '\0', result.content.size()) != nullptr) {
      result.content.clear();
      result.status = TextFileReadStatus::Binary;
      return result;
    }
  }
  result.status = TextFileReadStatus::Ok;
  return result;
}

bool ReadFileForTextSearch(const std::filesystem::path& path, std::string& out,
                           std::uintmax_t max_bytes) {
  // Count every entry (before any early-out) so a test can observe exactly how many
  // files replace-all touched. Relaxed is sufficient: the count is only read after
  // the concurrent search work has quiesced.
  g_text_search_read_count.fetch_add(1, std::memory_order_relaxed);
  // Reject non-regular files before opening so a special file under the project
  // tree cannot block a search worker on open/seek.
  if (!IsRegularFileFollowingSymlinks(path)) {
    return false;
  }
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }
  file.seekg(0, std::ios::end);
  const std::streamoff size = file.tellg();
  if (size < 0) {
    return false;
  }
  // Skip files too large to buffer (same OOM guard as ReadTextFile); an oversized
  // file is simply not searched rather than crashing the search worker. The default
  // cap (kMaxSearchFileBytes) is well below the whole-file cap so N search workers
  // cannot together hold multiple gigabytes.
  if (static_cast<std::uintmax_t>(size) > max_bytes) {
    return false;
  }
  file.seekg(0, std::ios::beg);

  // resize() keeps any capacity the caller already allocated, so reused buffers
  // only grow toward the largest file rather than reallocating per file.
  out.resize(static_cast<std::size_t>(size));
  if (size > 0) {
    file.read(out.data(), static_cast<std::streamsize>(size));
    if (!file) {
      return false;
    }
    // Any embedded NUL means the file is binary; skip it. memchr bails at the
    // first NUL, so binaries with an early NUL cost only one scan up to it.
    if (std::memchr(out.data(), '\0', out.size()) != nullptr) {
      return false;
    }
  }
  return true;
}

std::size_t TextSearchReadCount() {
  return g_text_search_read_count.load(std::memory_order_relaxed);
}

void ResetTextSearchReadCount() {
  g_text_search_read_count.store(0, std::memory_order_relaxed);
}

FileSignature StatFileSignature(const std::filesystem::path& path) {
  FileSignature signature;
  if (path.empty()) {
    return signature;  // exists=false, error=false
  }

  std::error_code error;
  const auto mtime = std::filesystem::last_write_time(path, error);
  if (error) {
    if (error == std::errc::no_such_file_or_directory) {
      return signature;  // absent, not an error
    }
    signature.error = true;
    return signature;
  }

  std::error_code size_error;
  const std::uintmax_t size = std::filesystem::file_size(path, size_error);
  if (size_error) {
    if (size_error == std::errc::no_such_file_or_directory) {
      return signature;  // vanished between the two stats
    }
    signature.error = true;
    return signature;
  }

  signature.exists = true;
  signature.mtime_ticks = static_cast<std::uint64_t>(mtime.time_since_epoch().count());
  signature.size = size;
  return signature;
}

// If `path` is a symlink, resolve it to the real file we should overwrite. An atomic
// temp+rename against the link path itself replaces the *link* with a regular file and
// never touches the intended target; resolving here means the rename lands on the target
// so the link is preserved and its content is updated. Falls back to `path` for a
// non-symlink, an unresolvable chain (loop), or any resolution error.
//
// The chain is followed manually rather than via weakly_canonical(): for a link whose
// target does not yet exist (e.g. `link -> subdir/missing.txt`), weakly_canonical stops
// at the last *existing* prefix — the link's parent — and appends the link's own name
// lexically, so it returns the link node itself. Saving there would then destroy the
// link. read_symlink + lexical resolution against the link's parent yields the intended
// target whether or not it exists.
std::filesystem::path ResolveSymlinkTarget(const std::filesystem::path& path) {
  std::error_code error;
  std::filesystem::path current = path;
  // Bound the walk so a cyclic link (`a -> b -> a`) cannot spin forever; 40 hops is far
  // beyond any legitimate link chain and matches typical kernel SYMLOOP_MAX.
  for (int hops = 0; hops < 40; ++hops) {
    // Key on is_symlink, not the error code: symlink_status on a not-yet-existing
    // target reports a not_found status (is_symlink == false) but may also set `error`
    // (ENOENT). Such a target is a correct write destination — a missing file, or the
    // final resolved target of a dangling link — so terminate the walk and return it.
    if (!std::filesystem::is_symlink(std::filesystem::symlink_status(current, error))) {
      return current;
    }
    error.clear();
    const std::filesystem::path link_target = std::filesystem::read_symlink(current, error);
    if (error || link_target.empty()) {
      // Unreadable link: fall back to the originally requested path (at worst this
      // overwrites the top-level link, the pre-existing behavior).
      return path;
    }
    current = (link_target.is_absolute() ? link_target
                                         : current.parent_path() / link_target)
                  .lexically_normal();
  }
  return path;  // loop guard tripped: keep the link node rather than chase a cycle
}

bool WriteTextFileAtomically(const std::filesystem::path& path, std::string_view text) {
  if (path.empty()) {
    return false;
  }

  // Overwrite the symlink's target, not the link node, so saving a symlinked file
  // preserves the link instead of clobbering it with a fresh regular file.
  const std::filesystem::path target = ResolveSymlinkTarget(path);

  std::error_code error;
  // Only create the parent when there is one. For a bare relative filename the parent
  // path is empty, and libstdc++'s create_directories("") sets EINVAL and would fail
  // every such save — mirror PersistedRecordWriter's guarded pattern instead.
  const std::filesystem::path parent = target.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent, error);
    if (error) {
      return false;
    }
  }

  // Snapshot the existing file's mode/ownership so the atomic replace does not silently
  // reset it to a fresh 0644 inode (dropping +x, setuid/setgid, group-write, or a
  // restrictive 0600). Empty/valid==false for a brand-new file, which then keeps 0644.
  const FilePermissions permissions = CaptureFilePermissions(target);

  // Unique per-write temp beside the target (same directory keeps the final rename
  // atomic on one filesystem) so two writers cannot O_TRUNC each other's staging file.
  const std::filesystem::path temp_path = UniqueTemporaryPath(target);
  std::filesystem::remove(temp_path, error);
  error.clear();

  // Durably write the temp file (fsync of contents) before swapping it into
  // place. The parent directory is intentionally NOT fsynced: it is the slow
  // half on networked filesystems and we keep document saves on the fast path
  // per the project's speed-first priority. The temp fsync still guarantees the
  // saved bytes survive a crash once the rename lands.
  const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(text.data()),
                                         text.size());
  if (!util::WriteFileBytesDurable(temp_path, bytes)) {
    std::filesystem::remove(temp_path, error);
    return false;
  }

  // Carry the original mode/owner onto the temp before it takes the target's place.
  ApplyFilePermissions(temp_path, permissions);

  if (!util::RenameReplacing(temp_path, target)) {
    std::filesystem::remove(temp_path, error);
    return false;
  }
  return true;
}

}  // namespace microide::util
