#include "project/remote/MirrorObjects.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <system_error>
#include <vector>

#include "util/TextFileIO.h"

namespace microide::project::remote {

std::filesystem::path MirrorObjects::PathOf(const util::ContentHash& hash) const {
  const std::string hex = hash.Hex();
  // Fanned out by the first byte, as git does: no directory of 50,000 entries.
  return directory_ / hex.substr(0, 2) / hex;
}

namespace {

// temp + rename, no fsync: a reader never sees a half-written object, and a torn
// one after a crash fails Get's hash check.
void WriteUnflushed(const std::filesystem::path& path, std::string_view bytes) {
#if defined(__unix__) || defined(__APPLE__)
  const std::string temp = path.string() + ".tmp";
  const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) {
    return;
  }
  std::size_t written = 0;
  while (written < bytes.size()) {
    const ssize_t count = ::write(fd, bytes.data() + written, bytes.size() - written);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      break;
    }
    written += static_cast<std::size_t>(count);
  }
  ::close(fd);
  if (written != bytes.size() || ::rename(temp.c_str(), path.c_str()) != 0) {
    ::unlink(temp.c_str());
  }
#else
  (void)util::WriteTextFileAtomically(path, bytes);
#endif
}

}  // namespace

void MirrorObjects::Put(const util::ContentHash& hash, std::string_view bytes) {
  if (bytes.size() > kMaxObjectBytes) {
    return;
  }
  const std::filesystem::path path = PathOf(hash);
  std::lock_guard lock(mutex_);
  std::error_code error;
  if (std::filesystem::exists(path, error)) {
    // Content-addressed: the same bytes. Touch it, so the sweep sees it as recent.
    std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now(), error);
    return;
  }
  std::filesystem::create_directories(path.parent_path(), error);
  // A failed write only means a later transfer: the store is a cache. Not flushed:
  // Get re-hashes every object and discards one a crash tore, so a flush per pulled
  // file (it was one, beside the tree's) bought nothing (TD-2026-10-14-340).
  WriteUnflushed(path, bytes);
}

std::optional<std::string> MirrorObjects::Get(const util::ContentHash& hash) {
  const std::filesystem::path path = PathOf(hash);
  std::optional<std::string> bytes = util::ReadTextFile(path);
  if (!bytes.has_value()) {
    return std::nullopt;
  }
  if (util::HashContent(*bytes) != hash) {
    std::lock_guard lock(mutex_);
    std::error_code error;
    std::filesystem::remove(path, error);  // damaged on disk: never served
    return std::nullopt;
  }
  return bytes;
}

bool MirrorObjects::Has(const util::ContentHash& hash) const {
  std::error_code error;
  return std::filesystem::is_regular_file(PathOf(hash), error);
}

MirrorObjects::SweepResult MirrorObjects::Sweep(const std::unordered_set<std::string>& reachable_hex,
                                                std::uint64_t budget_bytes) {
  struct Candidate {
    std::filesystem::path path;
    std::uint64_t bytes = 0;
    std::filesystem::file_time_type written;
  };
  std::lock_guard lock(mutex_);
  SweepResult result;
  std::vector<Candidate> unreachable;
  std::error_code error;
  for (auto it = std::filesystem::recursive_directory_iterator(directory_, error);
       !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
    if (!it->is_regular_file(error)) {
      continue;
    }
    const std::uint64_t bytes = it->file_size(error);
    result.kept_bytes += bytes;
    if (!reachable_hex.contains(it->path().filename().string())) {
      unreachable.push_back(Candidate{it->path(), bytes, it->last_write_time(error)});
    }
  }
  std::sort(unreachable.begin(), unreachable.end(),
            [](const Candidate& a, const Candidate& b) { return a.written < b.written; });
  for (const Candidate& candidate : unreachable) {
    if (result.kept_bytes <= budget_bytes) {
      break;
    }
    if (std::filesystem::remove(candidate.path, error)) {
      result.kept_bytes -= candidate.bytes;
      result.removed_bytes += candidate.bytes;
      ++result.removed;
    }
  }
  return result;
}

}  // namespace microide::project::remote
