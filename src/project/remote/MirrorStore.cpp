#include "project/remote/MirrorStore.h"

#include <cstring>
#include <functional>
#include <span>
#include <system_error>
#include <vector>

#include "persistence/PersistedRecordReader.h"
#include "persistence/PersistedRecordWriter.h"
#include "platform/AppDirectories.h"
#include "platform/RuntimePaths.h"
#include "util/ByteCodec.h"

#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#endif

namespace microide::project::remote {
namespace {

constexpr std::uint64_t kStateVersion = 1;

enum EntryFlag : std::uint8_t {
  kHasRemote = 1u << 0,
  kHasBase = 1u << 1,
  kLocalKnown = 1u << 2,
  kPushPending = 1u << 3,
  kConflict = 1u << 4,
  kLocalOnly = 1u << 5,
};

std::uint64_t ZigZag(std::int64_t value) {
  return (static_cast<std::uint64_t>(value) << 1) ^ static_cast<std::uint64_t>(value >> 63);
}
std::int64_t UnZigZag(std::uint64_t value) {
  return static_cast<std::int64_t>(value >> 1) ^ -static_cast<std::int64_t>(value & 1);
}

// A host string as a single safe path component.
std::string Component(std::string_view text) {
  std::string out;
  for (const char c : text) {
    const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == '.' || c == '-' || c == '_' || c == '@';
    out.push_back(safe ? c : '_');
  }
  if (out.empty() || out == "." || out == "..") {
    out = "_" + out;
  }
  return out.substr(0, 64);
}

struct LocalStat {
  bool exists = false;
  bool regular = false;
  std::uint64_t size = 0;
  std::int64_t mtime_ns = 0;
  std::int64_t ctime_ns = 0;

  // Whether this stat may stand for the content from now on (see IsRacySignature).
  bool Trustworthy() const {
    return exists && regular && !IsRacySignature(mtime_ns, ctime_ns, WallClockNowNs());
  }
};

LocalStat StatNoFollow(const std::filesystem::path& path) {
  LocalStat out;
#if defined(__unix__) || defined(__APPLE__)
  struct stat info {};
  if (::lstat(path.c_str(), &info) != 0) {
    return out;
  }
  out.exists = true;
  out.regular = S_ISREG(info.st_mode);
  out.size = static_cast<std::uint64_t>(info.st_size);
  out.mtime_ns = static_cast<std::int64_t>(info.st_mtim.tv_sec) * 1'000'000'000 + info.st_mtim.tv_nsec;
  out.ctime_ns = static_cast<std::int64_t>(info.st_ctim.tv_sec) * 1'000'000'000 + info.st_ctim.tv_nsec;
#else
  (void)path;
#endif
  return out;
}

}  // namespace

MirrorStore::MirrorStore(std::filesystem::path directory, std::string_view tree_name)
    : directory_(std::move(directory)),
      tree_(directory_ / Component(tree_name == "meta" ? "meta_" : tree_name)),
      meta_(directory_ / "meta") {}

std::filesystem::path MirrorStore::DefaultDirectory(std::string_view host,
                                                    std::string_view host_root) {
  const std::filesystem::path root_path{std::string(host_root)};
  std::string basename = root_path.filename().string();
  if (basename.empty()) {
    basename = root_path.parent_path().filename().string();
  }
  const std::string slug = Component(basename.empty() ? "root" : basename) + "-" +
                           util::HashContent(host_root).Hex().substr(0, 12);
  return platform::ResolveAppDirectory(platform::UserDirectoryKind::Data, "microide") / "remote" /
         Component(host) / slug;
}

bool MirrorStore::Open(std::string* error) {
  for (const std::filesystem::path& dir : {directory_, tree_, meta_}) {
    if (!platform::EnsureSecurePrivateDirectory(dir)) {
      *error = "cannot create the mirror directory " + dir.string();
      return false;
    }
  }
  entries_.clear();
  manifest_id_ = 0;
  persistence::PersistedRecordReaderError read_error = persistence::PersistedRecordReaderError::None;
  const auto record = persistence::PersistedRecordReader::ReadFile(state_path(), &read_error);
  if (!record.has_value()) {
    if (read_error == persistence::PersistedRecordReaderError::NotFound) {
      return true;  // a new mirror
    }
    *error = "the mirror's state file " + state_path().string() +
             " is unreadable; move it aside to start this mirror over";
    return false;
  }
  const std::string_view body(reinterpret_cast<const char*>(record->body.data()),
                              record->body.size());
  util::ByteReader in(body);
  const std::uint64_t version = in.Varint();
  manifest_id_ = in.Varint();
  const std::uint64_t count = in.Varint();
  if (in.failed() || version != kStateVersion || count > body.size()) {
    *error = "the mirror's state file " + state_path().string() + " has an unknown format";
    return false;
  }
  for (std::uint64_t i = 0; i < count && !in.failed(); ++i) {
    std::string path(in.Bytes(kMaxManifestPathBytes));
    const auto flags = static_cast<std::uint8_t>(in.Le(1));
    Entry entry;
    if ((flags & kHasRemote) != 0) {
      entry.has_remote = true;
      entry.remote.path = path;
      const std::uint64_t kind = in.Le(1);
      entry.remote.kind = static_cast<ManifestEntryKind>(
          kind > static_cast<std::uint64_t>(ManifestEntryKind::Submodule) ? 0 : kind);
      entry.remote.size = in.Varint();
      entry.remote.mode = static_cast<std::uint32_t>(in.Varint() & 07777);
      entry.remote.mtime_ns = UnZigZag(in.Varint());
      if (entry.remote.kind == ManifestEntryKind::File) {
        if (const auto hash = util::ContentHash::FromRaw(in.Raw(util::ContentHash::kBytes))) {
          entry.remote.hash = *hash;
        }
      } else if (entry.remote.kind == ManifestEntryKind::Symlink) {
        entry.remote.link_target = std::string(in.Bytes(kMaxManifestPathBytes));
      }
    }
    if ((flags & kHasBase) != 0) {
      entry.base = util::ContentHash::FromRaw(in.Raw(util::ContentHash::kBytes));
    }
    if ((flags & kLocalKnown) != 0) {
      entry.local_known = true;
      entry.local_size = in.Varint();
      entry.local_mtime_ns = UnZigZag(in.Varint());
    }
    entry.push_pending = (flags & kPushPending) != 0;
    entry.conflict = (flags & kConflict) != 0;
    entry.local_only = (flags & kLocalOnly) != 0;
    if (!IsSafeRelativePath(path)) {
      in.Fail();
      break;
    }
    entries_.emplace(std::move(path), std::move(entry));
  }
  if (in.failed() || !in.at_end()) {
    entries_.clear();
    *error = "the mirror's state file " + state_path().string() + " is truncated or corrupt";
    return false;
  }
  return true;
}

bool MirrorStore::Save(std::string* error) const {
  std::string body;
  util::PutVarint(body, kStateVersion);
  util::PutVarint(body, manifest_id_);
  util::PutVarint(body, entries_.size());
  for (const auto& [path, entry] : entries_) {
    util::PutBytes(body, path);
    std::uint8_t flags = 0;
    flags |= entry.has_remote ? kHasRemote : 0;
    flags |= entry.base.has_value() ? kHasBase : 0;
    flags |= entry.local_known ? kLocalKnown : 0;
    flags |= entry.push_pending ? kPushPending : 0;
    flags |= entry.conflict ? kConflict : 0;
    flags |= entry.local_only ? kLocalOnly : 0;
    body.push_back(static_cast<char>(flags));
    if (entry.has_remote) {
      body.push_back(static_cast<char>(entry.remote.kind));
      util::PutVarint(body, entry.remote.size);
      util::PutVarint(body, entry.remote.mode);
      util::PutVarint(body, ZigZag(entry.remote.mtime_ns));
      if (entry.remote.kind == ManifestEntryKind::File) {
        body.append(entry.remote.hash.raw());
      } else if (entry.remote.kind == ManifestEntryKind::Symlink) {
        util::PutBytes(body, entry.remote.link_target);
      }
    }
    if (entry.base.has_value()) {
      body.append(entry.base->raw());
    }
    if (entry.local_known) {
      util::PutVarint(body, entry.local_size);
      util::PutVarint(body, ZigZag(entry.local_mtime_ns));
    }
  }
  persistence::PersistedRecordWriterError write_error = persistence::PersistedRecordWriterError::None;
  if (!persistence::PersistedRecordWriter::WriteFile(
          state_path(),
          std::span<const std::byte>(reinterpret_cast<const std::byte*>(body.data()), body.size()),
          0, &write_error)) {
    *error = "cannot write the mirror's state file " + state_path().string();
    return false;
  }
  return true;
}

MirrorStore::Entry* MirrorStore::Find(std::string_view path) {
  const auto it = entries_.find(path);
  return it == entries_.end() ? nullptr : &it->second;
}

MirrorStore::LocalState MirrorStore::CheckLocal(std::string_view path, Entry& entry) const {
  const std::filesystem::path local = tree_ / std::string(path);
  const LocalStat now = StatNoFollow(local);
  if (!now.exists) {
    return LocalState::Missing;
  }
  if (!entry.base.has_value() || !now.regular) {
    return LocalState::Differs;
  }
  if (entry.local_known && entry.local_size == now.size && entry.local_mtime_ns == now.mtime_ns) {
    return LocalState::MatchesBase;
  }
  std::uint64_t size = 0;
  const auto hash = util::HashFileContent(local, &size);
  if (hash.has_value() && *hash == *entry.base) {
    // Touched, or rewritten with the same bytes: re-record so the next check is a
    // stat — unless the stat is too recent to vouch for the bytes.
    entry.local_known = now.Trustworthy();
    entry.local_size = now.size;
    entry.local_mtime_ns = now.mtime_ns;
    return LocalState::MatchesBase;
  }
  return LocalState::Differs;
}

void MirrorStore::RecordLocal(std::string_view path, Entry& entry) const {
  const LocalStat now = StatNoFollow(tree_ / std::string(path));
  entry.local_known = now.Trustworthy();
  entry.local_size = now.size;
  entry.local_mtime_ns = now.mtime_ns;
}

std::mutex& MirrorStore::PathLock(std::string_view path) {
  return path_locks_[std::hash<std::string_view>{}(path) % path_locks_.size()];
}

}  // namespace microide::project::remote
