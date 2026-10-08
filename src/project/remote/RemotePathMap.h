#pragma once

#include <filesystem>
#include <optional>

namespace microide::project::remote {

// The two-way map between the editor's tree (the mirror, in a remote project) and
// the host's tree, component-wise. A path outside a root is not the project's and
// maps to nothing. One implementation for the real remote launcher and the parity
// suite's loopback stand-in, so the stand-in tests the map that ships.
class RemotePathMap {
 public:
  RemotePathMap(std::filesystem::path local_root, std::filesystem::path host_root);
  std::optional<std::filesystem::path> ToHost(const std::filesystem::path& local_path) const;
  std::optional<std::filesystem::path> ToLocal(const std::filesystem::path& host_path) const;
  const std::filesystem::path& local_root() const { return local_root_; }
  const std::filesystem::path& host_root() const { return host_root_; }

 private:
  std::filesystem::path local_root_;
  std::filesystem::path host_root_;
};

}  // namespace microide::project::remote
