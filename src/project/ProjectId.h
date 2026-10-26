#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <variant>

namespace microide::project {

// What NAMES a project (dev-docs/design/remote-projects.md § 6.8, Groundwork G8):
// a local directory, or a directory on a host. Never both -- a closed variant, so
// "local or remote" cannot be two fields that disagree.
//
// It is not the directory the editor does I/O in. That is `IoRoot()`: the path for
// a local project, the mirror's tree for a remote one, and it stays a path on
// `ProjectWorkspaceState::root` for the ~170 file, git, search and process sites.
// Everything that SHOWS a project goes through `Label()`/`Location()`, so a remote
// project's mirror path under the data directory has no way onto the screen.
//
// Fixed when a project's catalog entry is created (an open, a session restore, a
// recents entry) and carried through every reset of its scoped state, like its
// locality.
struct ProjectId {
  struct Local {
    std::filesystem::path root;
    friend bool operator==(const Local&, const Local&) = default;
  };
  struct Remote {
    std::string host;       // `[user@]host[:port]`, as RemoteHostTarget::Display
    std::string host_root;  // absolute, on the host
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
  // No project: a local id with no path (the welcome state's).
  [[nodiscard]] bool empty() const { return local() != nullptr && local()->root.empty(); }

  // The short name: the directory's basename -- the host root's for a remote
  // project -- and the normalized path for a filesystem root. "" when empty().
  [[nodiscard]] std::string Label() const;
  // The full name: the normalized path, or `host:/abs/path`.
  [[nodiscard]] std::string Location() const;
  // The directory every I/O site uses: the path, or the mirror's default tree.
  [[nodiscard]] std::filesystem::path IoRoot() const;

  friend bool operator==(const ProjectId&, const ProjectId&) = default;
};

// The id of the project whose I/O root is `io_root`: Remote exactly when it is the
// default tree of the mirror whose `meta/remote` record names it, Local otherwise
// (a copy of a mirror, or a renamed one, is an ordinary folder). Reads that record:
// it runs when a catalog entry is created from a bare path, never per frame.
ProjectId ResolveProjectId(const std::filesystem::path& io_root);

}  // namespace microide::project
