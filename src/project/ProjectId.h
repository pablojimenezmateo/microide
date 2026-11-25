#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <variant>

namespace microide::project {

struct ProjectId {
  struct Local {
    std::filesystem::path root;
    friend bool operator==(const Local&, const Local&) = default;
  };
  struct Remote {
    std::string host;
    std::string host_root;
    friend bool operator==(const Remote&, const Remote&) = default;
  };
  std::variant<Local, Remote> location;

  static ProjectId ForLocal(std::filesystem::path root) { return ProjectId{Local{std::move(root)}}; }
  static ProjectId ForRemote(std::string host, std::string host_root) {
    return ProjectId{Remote{std::move(host), std::move(host_root)}};
  }

  [[nodiscard]] bool IsRemote() const { return std::holds_alternative<Remote>(location); }
  [[nodiscard]] const Remote* remote() const { return std::get_if<Remote>(&location); }
  [[nodiscard]] const Local* local() const { return std::get_if<Local>(&location); }
  [[nodiscard]] bool empty() const { return local() != nullptr && local()->root.empty(); }

  [[nodiscard]] std::string Label() const;
  [[nodiscard]] std::string Location() const;
  [[nodiscard]] std::filesystem::path IoRoot() const;

  friend bool operator==(const ProjectId&, const ProjectId&) = default;
};

ProjectId ResolveProjectId(const std::filesystem::path& io_root);

}
