#include "project/remote/RemoteManifest.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "util/ByteCodec.h"

namespace microide::project::remote {
namespace {

using util::ByteReader;
using util::PutBytes;
using util::PutVarint;

constexpr std::size_t kMaxComponentBytes = 255;

std::uint64_t ZigZag(std::int64_t value) {
  return (static_cast<std::uint64_t>(value) << 1) ^ static_cast<std::uint64_t>(value >> 63);
}

std::int64_t UnZigZag(std::uint64_t value) {
  return static_cast<std::int64_t>(value >> 1) ^ -static_cast<std::int64_t>(value & 1);
}

}  // namespace

std::int64_t WallClockNowNs() {
  // File timestamps are wall-clock time, so this is the clock to compare them with.
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

bool IsSafeRelativePath(std::string_view path) {
  if (path.empty() || path.size() > kMaxManifestPathBytes || path.front() == '/') {
    return false;
  }
  std::size_t start = 0;
  while (start <= path.size()) {
    const std::size_t end = std::min(path.find('/', start), path.size());
    const std::string_view component = path.substr(start, end - start);
    if (component.empty() || component == "." || component == ".." ||
        component.size() > kMaxComponentBytes ||
        component.find_first_of(std::string_view("\0\\", 2)) != std::string_view::npos) {
      return false;
    }
    start = end + 1;
  }
  return true;
}

namespace {

template <typename RowAt>
void EncodeRows(std::size_t count, const RowAt& row_at, std::string& out) {
  PutVarint(out, count);
  std::string_view previous;
  for (std::size_t i = 0; i < count; ++i) {
    const ManifestRow& row = row_at(i);
    const std::size_t limit = std::min(previous.size(), row.path.size());
    std::size_t shared = 0;
    while (shared < limit && previous[shared] == row.path[shared]) {
      ++shared;
    }
    PutVarint(out, shared);
    PutBytes(out, std::string_view(row.path).substr(shared));
    out.push_back(static_cast<char>(row.kind));
    PutVarint(out, row.size);
    PutVarint(out, row.mode);
    PutVarint(out, ZigZag(row.mtime_ns));
    if (row.kind == ManifestEntryKind::File) {
      out.append(row.hash.raw());
    } else if (row.kind == ManifestEntryKind::Symlink) {
      PutBytes(out, row.link_target);
    }
    previous = row.path;
  }
}

}  // namespace

void EncodeManifestRows(const ManifestRow* rows, std::size_t count, std::string& out) {
  EncodeRows(count, [rows](std::size_t i) -> const ManifestRow& { return rows[i]; }, out);
}

bool DecodeManifestRows(std::string_view bytes, std::vector<ManifestRow>& rows) {
  const std::size_t original = rows.size();
  ByteReader in(bytes);
  const std::uint64_t count = in.Varint();
  // Each row costs at least five bytes, so a count the payload cannot hold is a
  // forgery and must not size a reservation.
  if (in.failed() || count > in.remaining() / 5 + 1) {
    return false;
  }
  rows.reserve(original + static_cast<std::size_t>(count));
  const auto fail = [&]() {
    rows.resize(original);
    return false;
  };
  for (std::uint64_t i = 0; i < count; ++i) {
    const std::uint64_t shared = in.Varint();
    const std::size_t previous_size = rows.size() == original ? 0 : rows.back().path.size();
    if (in.failed() || shared > previous_size) {
      return fail();
    }
    const std::string_view suffix = in.Bytes(kMaxManifestPathBytes);
    ManifestRow row;
    if (!in.failed()) {
      row.path.reserve(static_cast<std::size_t>(shared) + suffix.size());
      if (shared > 0) {
        row.path.assign(rows.back().path, 0, static_cast<std::size_t>(shared));
      }
      row.path.append(suffix);
    }
    const std::uint64_t kind = in.Le(1);
    row.size = in.Varint();
    const std::uint64_t mode = in.Varint();
    row.mtime_ns = UnZigZag(in.Varint());
    if (in.failed() || kind > static_cast<std::uint64_t>(ManifestEntryKind::Submodule) ||
        mode > 07777 || !IsSafeRelativePath(row.path)) {
      return fail();
    }
    row.kind = static_cast<ManifestEntryKind>(kind);
    row.mode = static_cast<std::uint32_t>(mode);
    if (row.kind == ManifestEntryKind::File) {
      const std::string_view raw = in.Raw(util::ContentHash::kBytes);
      if (in.failed()) {
        return fail();
      }
      std::memcpy(row.hash.bytes.data(), raw.data(), raw.size());
    } else if (row.kind == ManifestEntryKind::Symlink) {
      row.link_target = std::string(in.Bytes(kMaxManifestPathBytes));
      if (in.failed() || row.link_target.empty() ||
          row.link_target.find('\0') != std::string::npos) {
        return fail();
      }
    }
    rows.push_back(std::move(row));
  }
  if (in.failed() || !in.at_end()) {
    return fail();
  }
  return true;
}

void DiffManifests(const std::vector<ManifestRow>& before, const std::vector<ManifestRow>& after,
                   std::vector<const ManifestRow*>& changed, std::vector<const std::string*>& deleted) {
  std::size_t i = 0;
  std::size_t j = 0;
  while (i < before.size() || j < after.size()) {
    if (j == after.size() || (i < before.size() && before[i].path < after[j].path)) {
      deleted.push_back(&before[i++].path);
    } else if (i == before.size() || after[j].path < before[i].path) {
      changed.push_back(&after[j++]);
    } else {
      if (!(before[i] == after[j])) {
        changed.push_back(&after[j]);
      }
      ++i;
      ++j;
    }
  }
}

void EncodeWatchDelta(const std::string* const* deleted, std::size_t deleted_count,
                      const ManifestRow* const* rows, std::size_t row_count, std::string& out) {
  PutVarint(out, deleted_count);
  for (std::size_t i = 0; i < deleted_count; ++i) {
    PutBytes(out, *deleted[i]);
  }
  EncodeRows(row_count, [rows](std::size_t i) -> const ManifestRow& { return *rows[i]; }, out);
}

bool DecodeWatchDelta(std::string_view bytes, std::vector<std::string>& deleted,
                      std::vector<ManifestRow>& rows) {
  ByteReader in(bytes);
  const std::uint64_t count = in.Varint();
  if (in.failed() || count > bytes.size()) {
    return false;
  }
  std::vector<std::string> paths;
  paths.reserve(static_cast<std::size_t>(count));
  for (std::uint64_t i = 0; i < count; ++i) {
    std::string path(in.Bytes(kMaxManifestPathBytes));
    if (in.failed() || !IsSafeRelativePath(path)) {
      return false;
    }
    paths.push_back(std::move(path));
  }
  const std::size_t consumed = bytes.size() - in.remaining();
  if (!DecodeManifestRows(bytes.substr(consumed), rows)) {
    return false;
  }
  deleted.insert(deleted.end(), std::make_move_iterator(paths.begin()),
                 std::make_move_iterator(paths.end()));
  return true;
}

}  // namespace microide::project::remote
