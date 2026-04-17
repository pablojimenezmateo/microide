#pragma once

#include <SDL3/SDL.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "render/Theme.h"

namespace microide::workspace {

struct WorkspaceTabTextModel {
  std::string display_title;
  std::string tooltip_label;
};

std::string ProjectStateDirectoryName(const std::filesystem::path& project_root);
WorkspaceTabTextModel BuildWorkspaceTabTextModel(const std::filesystem::path& project_root,
                                                 const std::filesystem::path& path,
                                                 std::string_view fallback_title,
                                                 bool dirty);
std::string BuildEditorBreadcrumbLabel(const std::filesystem::path& project_root,
                                       const std::filesystem::path& path,
                                       bool placeholder);
std::string BuildCompareBreadcrumbLabel(const std::filesystem::path& project_root,
                                        const std::filesystem::path& path,
                                        std::string_view left_label,
                                        std::string_view right_label);
std::string BuildMergeBreadcrumbLabel(const std::filesystem::path& project_root,
                                      const std::filesystem::path& output_path,
                                      std::string_view incoming_label,
                                      std::string_view current_label);
std::optional<SDL_Color> ParseProjectColor(std::string_view text);
std::string FormatProjectColor(SDL_Color color);
SDL_Color DefaultProjectBaseColor(const std::filesystem::path& project_root);
void ApplyProjectAccent(render::Theme& theme, SDL_Color accent);

}  // namespace microide::workspace
