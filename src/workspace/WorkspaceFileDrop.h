#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace microide::workspace {

enum class FileDropAction {
  None,
  OpenFile,
  OpenProject,
  OpenProjectThenFile,
};

struct FileDropRequest {
  FileDropAction action = FileDropAction::None;
  std::filesystem::path project_root;
  std::filesystem::path file_path;
};

FileDropRequest ResolveFileDrop(const std::filesystem::path& dropped, bool has_project_root);

struct FileDropOperations {
  std::function<bool(const std::filesystem::path&)> open_project;
  std::function<void(const std::filesystem::path&)> open_file;
};

bool ApplyFileDrop(const FileDropRequest& request, const FileDropOperations& operations);

std::string TerminalDropText(const std::filesystem::path& path_on_shell_machine);

}
