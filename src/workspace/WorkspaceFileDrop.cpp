#include "workspace/WorkspaceFileDrop.h"

#include <system_error>

namespace microide::workspace {

FileDropRequest ResolveFileDrop(const std::filesystem::path& dropped, bool has_project_root) {
  if (dropped.empty()) {
    return {};
  }

  // Every filesystem query here is the non-throwing overload: a drop can name a
  // dangling symlink, a path on a filesystem that just went away, or a URI SDL
  // decoded into something unopenable, and none of those should propagate an
  // exception out of the event loop.
  std::error_code ec;
  const std::filesystem::path path = std::filesystem::absolute(dropped, ec).lexically_normal();
  if (ec) {
    return {};
  }

  if (std::filesystem::is_directory(path, ec) && !ec) {
    return FileDropRequest{
        .action = FileDropAction::OpenProject,
        .project_root = path,
    };
  }

  // is_regular_file rather than exists: a drop of a fifo, socket or device node
  // would otherwise be handed to the editor, which then blocks opening it.
  if (!std::filesystem::is_regular_file(path, ec) || ec) {
    return {};
  }

  if (has_project_root) {
    return FileDropRequest{
        .action = FileDropAction::OpenFile,
        .file_path = path,
    };
  }

  std::filesystem::path parent = path.parent_path();
  if (parent.empty()) {
    parent = std::filesystem::current_path(ec);
    if (ec) {
      return {};
    }
  }
  return FileDropRequest{
      .action = FileDropAction::OpenProjectThenFile,
      .project_root = parent,
      .file_path = path,
  };
}

bool ApplyFileDrop(const FileDropRequest& request, const FileDropOperations& operations) {
  switch (request.action) {
    case FileDropAction::None:
      return false;
    case FileDropAction::OpenProject:
      return operations.open_project && operations.open_project(request.project_root);
    case FileDropAction::OpenFile:
      if (!operations.open_file) {
        return false;
      }
      operations.open_file(request.file_path);
      return true;
    case FileDropAction::OpenProjectThenFile:
      if (!operations.open_project || !operations.open_file) {
        return false;
      }
      // Stop on a failed project open rather than opening the file anyway: the
      // shell would still be pointed at whatever was (or was not) open before, and
      // a tab in the wrong project is worse than an ignored drop.
      if (!operations.open_project(request.project_root)) {
        return false;
      }
      operations.open_file(request.file_path);
      return true;
  }
  return false;
}

}  // namespace microide::workspace
