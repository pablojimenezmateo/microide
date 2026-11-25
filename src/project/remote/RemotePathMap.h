#pragma once

#include <filesystem>
#include <optional>

namespace microide::project::remote {

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

}
