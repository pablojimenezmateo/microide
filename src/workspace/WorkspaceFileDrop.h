#pragma once

// What a file dropped onto the window should do.
//
// The decision is a pure function of (dropped path, whether a project is open) so
// it can be tested without an SDL event, a window, or a shell. The SDL glue in
// WorkspaceEventOrchestrator only translates the result into calls.
//
// Behaviour follows VSCode, adapted to the fact that this shell needs a project
// root before it can open an editor tab:
//
//   directory                     -> open it as the project
//   file, project already open    -> open it as an editor tab, even when the file
//                                    lives outside the current root (VSCode opens
//                                    the file in the current window regardless, and
//                                    OpenPath imposes no containment rule)
//   file, no project open         -> open its parent directory as the project, then
//                                    open the file; without this the Open action
//                                    rejects with "No active project" and a drop
//                                    onto the welcome screen does nothing
//   anything that does not exist  -> nothing

#include <filesystem>
#include <functional>

namespace microide::workspace {

enum class FileDropAction {
  None,
  OpenFile,
  OpenProject,
  OpenProjectThenFile,
};

struct FileDropRequest {
  FileDropAction action = FileDropAction::None;
  // Set for OpenProject and OpenProjectThenFile.
  std::filesystem::path project_root;
  // Set for OpenFile and OpenProjectThenFile.
  std::filesystem::path file_path;
};

// `dropped` is the raw path from SDL_EVENT_DROP_FILE. Existence is resolved
// through the filesystem, so this is not constexpr-pure, but it takes no shell
// state beyond `has_project_root`.
FileDropRequest ResolveFileDrop(const std::filesystem::path& dropped, bool has_project_root);

// The two host calls a drop can turn into. Kept as an explicit port rather than a
// WorkspaceShell reference: this is the whole reason the drop logic can be tested,
// and it keeps the shell from gaining a member for a feature that is really two
// existing operations in sequence.
struct FileDropOperations {
  std::function<bool(const std::filesystem::path&)> open_project;
  std::function<void(const std::filesystem::path&)> open_file;
};

// Executes a resolved request. Returns whether anything was done, which is what
// the event dispatcher reports as "handled". A failed project open stops the
// sequence — opening the file into the previous project's shell state would be
// worse than doing nothing.
bool ApplyFileDrop(const FileDropRequest& request, const FileDropOperations& operations);

}  // namespace microide::workspace
