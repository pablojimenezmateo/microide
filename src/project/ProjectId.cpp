#include "project/ProjectId.h"

#include <optional>
#include <string_view>

#include "project/remote/RemoteHostSession.h"
#include "project/remote/RemoteProject.h"
#include "util/PathMatch.h"

namespace microide::project {
namespace {

// The last named component: `/a/b` and `/a/b/` are both `b`; `/` has none and is
// shown whole. A scan of the normalized text, so one allocation: the result.
std::string LeafLabel(const std::filesystem::path& path) {
  std::filesystem::path scratch;
  std::string_view text = util::NormalizedPathView(path, scratch).native();
  while (text.size() > 1 && text.back() == '/') {
    text.remove_suffix(1);
  }
  const std::size_t slash = text.rfind('/');
  const std::string_view name = slash == std::string_view::npos ? text : text.substr(slash + 1);
  return std::string(name.empty() ? text : name);
}

}  // namespace

std::string ProjectId::Label() const {
  if (const Remote* remote_id = remote()) {
    return LeafLabel(std::filesystem::path(remote_id->host_root));
  }
  const std::filesystem::path& root = local()->root;
  return root.empty() ? std::string() : LeafLabel(root);
}

std::string ProjectId::Location() const {
  if (const Remote* remote_id = remote()) {
    return remote_id->host + ":" + remote_id->host_root;
  }
  std::filesystem::path scratch;
  return util::NormalizedPathView(local()->root, scratch).string();
}

std::filesystem::path ProjectId::IoRoot() const {
  if (const Remote* remote_id = remote()) {
    return remote::RemoteProject::DefaultTree(remote_id->host, remote_id->host_root);
  }
  return local()->root;
}

ProjectId ResolveProjectId(const std::filesystem::path& io_root) {
  const std::optional<remote::RemoteProjectRecord> record = remote::RemoteProject::ReadRecord(io_root);
  if (record.has_value()) {
    const std::optional<remote::RemoteHostTarget> target =
        remote::ParseRemoteHostTarget(record->host, nullptr);
    // Only the mirror's own tree: a copy or a renamed directory is a local folder.
    if (target.has_value() &&
        remote::RemoteProject::DefaultTree(target->Display(), record->host_root) == io_root) {
      return ProjectId::ForRemote(target->Display(), record->host_root);
    }
  }
  return ProjectId::ForLocal(io_root);
}

}  // namespace microide::project
