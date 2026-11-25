#include "project/remote/FileVersionHistory.h"

#include <algorithm>
#include <optional>
#include <utility>

#include "util/ByteCodec.h"

namespace microide::project::remote {
namespace {

constexpr std::uint64_t kFormatVersion = 1;

}

void FileVersionHistory::Record(std::string_view path, const util::ContentHash& hash, std::uint64_t replaced_at) {
  if (path.empty() || path.size() > kMaxPathBytes) {
    return;
  }
  auto it = versions_.find(path);
  if (it == versions_.end()) {
    if (versions_.size() >= kMaxPaths) {
      EvictOldestPath();
    }
    it = versions_.emplace(std::string(path), std::vector<Version>{}).first;
  }
  std::vector<Version>& versions = it->second;
  if (!versions.empty() && versions.front().hash == hash) {
    versions.front().replaced_at = replaced_at;
    return;
  }
  versions.insert(versions.begin(), Version{.hash = hash, .replaced_at = replaced_at});
  if (versions.size() > kMaxVersionsPerPath) {
    versions.resize(kMaxVersionsPerPath);
  }
}

std::vector<FileVersionHistory::Version> FileVersionHistory::VersionsOf(std::string_view path) const {
  const auto it = versions_.find(path);
  return it == versions_.end() ? std::vector<Version>{} : it->second;
}

void FileVersionHistory::EvictOldestPath() {
  const auto oldest = std::min_element(versions_.begin(), versions_.end(), [](const auto& a, const auto& b) {
    return a.second.front().replaced_at < b.second.front().replaced_at;
  });
  if (oldest != versions_.end()) {
    versions_.erase(oldest);
  }
}

std::string FileVersionHistory::Encode() const {
  std::string out;
  util::PutVarint(out, kFormatVersion);
  util::PutVarint(out, versions_.size());
  for (const auto& [path, versions] : versions_) {
    util::PutBytes(out, path);
    util::PutVarint(out, versions.size());
    for (const Version& version : versions) {
      out.append(version.hash.raw());
      util::PutVarint(out, version.replaced_at);
    }
  }
  return out;
}

FileVersionHistory FileVersionHistory::Decode(std::string_view bytes) {
  FileVersionHistory history;
  util::ByteReader in(bytes);
  if (in.Varint() != kFormatVersion) {
    return history;
  }
  const std::uint64_t paths = in.Varint();
  for (std::uint64_t i = 0; i < paths && !in.failed() && history.versions_.size() < kMaxPaths; ++i) {
    std::string path(in.Bytes(kMaxPathBytes));
    const std::uint64_t count = in.Varint();
    std::vector<Version> versions;
    for (std::uint64_t j = 0; j < count && !in.failed(); ++j) {
      const std::optional<util::ContentHash> hash = util::ContentHash::FromRaw(in.Raw(util::ContentHash::kBytes));
      const std::uint64_t replaced_at = in.Varint();
      if (in.failed() || !hash.has_value()) {
        break;
      }
      if (versions.size() < kMaxVersionsPerPath) {
        versions.push_back(Version{.hash = *hash, .replaced_at = replaced_at});
      }
    }
    if (!path.empty() && !versions.empty()) {
      history.versions_.insert_or_assign(std::move(path), std::move(versions));
    }
  }
  return history;
}

}
