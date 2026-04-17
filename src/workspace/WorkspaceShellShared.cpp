#include "workspace/WorkspaceShellShared.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <sstream>

#include "workspace/WorkspacePathUtils.h"

namespace microide::workspace {

namespace {

std::uint64_t StablePathHash(std::string_view text) {
  std::uint64_t hash = 1469598103934665603ull;
  for (unsigned char c : text) {
    hash ^= static_cast<std::uint64_t>(c);
    hash *= 1099511628211ull;
  }
  return hash;
}

std::string HashToHex(std::uint64_t value) {
  static constexpr std::string_view kDigits = "0123456789abcdef";
  std::string hex(16, '0');
  for (int i = 15; i >= 0; --i) {
    hex[static_cast<std::size_t>(i)] = kDigits[value & 0xfu];
    value >>= 4;
  }
  return hex;
}

}  // namespace

GitSidebarEntryTextModel BuildGitSidebarEntryTextModel(const std::filesystem::path& relative_path,
                                                       bool staged) {
  const std::filesystem::path normalized_path = relative_path.lexically_normal();

  GitSidebarEntryTextModel model;
  model.primary_label = normalized_path.filename().string();
  if (model.primary_label.empty()) {
    model.primary_label = normalized_path.empty() ? "." : normalized_path.string();
  }

  const std::filesystem::path parent = normalized_path.parent_path();
  if (!parent.empty() && parent != ".") {
    model.secondary_label = parent.string();
  }
  if (staged) {
    if (!model.secondary_label.empty()) {
      model.secondary_label += "  ";
    }
    model.secondary_label += "[staged]";
  }
  return model;
}

WorkspaceTabTextModel BuildWorkspaceTabTextModel(const std::filesystem::path& project_root,
                                                 const std::filesystem::path& path,
                                                 std::string_view fallback_title,
                                                 bool dirty) {
  std::string title = path.filename().string();
  if (title.empty()) {
    title = fallback_title.empty() ? "untitled" : std::string(fallback_title);
  }

  WorkspaceTabTextModel model;
  model.display_title = dirty ? "*" + title : title;
  model.tooltip_label =
      path.empty() ? (fallback_title.empty() ? "untitled" : std::string(fallback_title))
                   : RelativePathLabel(project_root, path);
  return model;
}

std::string BuildEditorBreadcrumbLabel(const std::filesystem::path& project_root,
                                       const std::filesystem::path& path,
                                       bool placeholder) {
  if (path.empty()) {
    return placeholder ? "welcome" : "untitled";
  }

  return RelativePathLabel(project_root, path);
}

std::string BuildCompareBreadcrumbLabel(const std::filesystem::path& project_root,
                                        const std::filesystem::path& path,
                                        std::string_view left_label,
                                        std::string_view right_label) {
  return RelativePathLabel(project_root, path) + "  |  " + std::string(left_label) + " -> " +
         std::string(right_label);
}

std::string BuildMergeBreadcrumbLabel(const std::filesystem::path& project_root,
                                      const std::filesystem::path& output_path,
                                      std::string_view incoming_label,
                                      std::string_view current_label) {
  return RelativePathLabel(project_root, output_path) + "  |  " + std::string(incoming_label) +
         " -> " + std::string(current_label);
}

std::vector<GitSidebarLineSpec> BuildGitSidebarLineSpecs(
    const std::vector<GitSidebarSection>& entry_sections,
    bool git_repo_available,
    std::string_view git_base_ref,
    std::string_view git_base_label) {
  std::vector<GitSidebarLineSpec> lines;
  std::size_t modified_count = 0;
  std::size_t outgoing_count = 0;
  for (GitSidebarSection section : entry_sections) {
    if (section == GitSidebarSection::Modified) {
      ++modified_count;
    } else {
      ++outgoing_count;
    }
  }

  lines.push_back(GitSidebarLineSpec{
      .kind = GitSidebarLineKind::Header,
      .section = GitSidebarSection::Modified,
      .label = "Changes (" + std::to_string(modified_count) + ")",
  });
  if (modified_count == 0) {
    lines.push_back(GitSidebarLineSpec{
        .kind = GitSidebarLineKind::Empty,
        .section = GitSidebarSection::Modified,
        .label = git_repo_available ? "Working tree is clean" : "Not a git repository",
    });
  } else {
    for (std::size_t i = 0; i < entry_sections.size(); ++i) {
      if (entry_sections[i] != GitSidebarSection::Modified) {
        continue;
      }
      lines.push_back(GitSidebarLineSpec{
          .kind = GitSidebarLineKind::Entry,
          .section = GitSidebarSection::Modified,
          .label = {},
          .entry_index = static_cast<int>(i),
      });
    }
  }

  const std::string outgoing_header =
      git_base_label.empty()
          ? "Outgoing files (" + std::to_string(outgoing_count) + ")"
          : "Outgoing files (" + std::to_string(outgoing_count) + ")  " + std::string(git_base_label);
  lines.push_back(GitSidebarLineSpec{
      .kind = GitSidebarLineKind::Header,
      .section = GitSidebarSection::Outgoing,
      .label = outgoing_header,
  });
  if (outgoing_count == 0) {
    lines.push_back(GitSidebarLineSpec{
        .kind = GitSidebarLineKind::Empty,
        .section = GitSidebarSection::Outgoing,
        .label = git_base_ref.empty() ? "Base branch unavailable" : "No outgoing files",
    });
  } else {
    for (std::size_t i = 0; i < entry_sections.size(); ++i) {
      if (entry_sections[i] != GitSidebarSection::Outgoing) {
        continue;
      }
      lines.push_back(GitSidebarLineSpec{
          .kind = GitSidebarLineKind::Entry,
          .section = GitSidebarSection::Outgoing,
          .label = {},
          .entry_index = static_cast<int>(i),
      });
    }
  }

  return lines;
}

std::optional<std::size_t> FindSelectedGitSidebarLineIndex(
    const std::vector<GitSidebarLineSpec>& lines,
    std::size_t selected_entry_index) {
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (lines[i].kind == GitSidebarLineKind::Entry &&
        lines[i].entry_index == static_cast<int>(selected_entry_index)) {
      return i;
    }
  }
  return std::nullopt;
}

std::vector<int> BuildProjectSearchResultLineMap(
    const std::vector<project::ProjectSearchResult>& results) {
  std::vector<int> line_map;
  line_map.reserve(results.size() * 2);

  std::filesystem::path current_path;
  for (std::size_t i = 0; i < results.size(); ++i) {
    const auto& result = results[i];
    if (result.relative_path != current_path) {
      current_path = result.relative_path;
      line_map.push_back(-1);
    }
    line_map.push_back(static_cast<int>(i));
  }

  return line_map;
}

int FindProjectSearchResultLine(const std::vector<int>& line_map, std::size_t result_index) {
  for (std::size_t line = 0; line < line_map.size(); ++line) {
    if (line_map[line] == static_cast<int>(result_index)) {
      return static_cast<int>(line);
    }
  }
  return 0;
}

std::string ProjectStateDirectoryName(const std::filesystem::path& project_root) {
  const std::string label =
      project_root.filename().empty() ? "project" : project_root.filename().string();
  std::string sanitized;
  sanitized.reserve(label.size());
  for (const unsigned char c : label) {
    if (std::isalnum(c) != 0 || c == '-' || c == '_') {
      sanitized.push_back(static_cast<char>(c));
    } else {
      sanitized.push_back('-');
    }
  }
  if (sanitized.empty()) {
    sanitized = "project";
  }
  return sanitized + "-" + HashToHex(StablePathHash(project_root.lexically_normal().string()));
}

std::optional<SDL_Color> ParseProjectColor(std::string_view text) {
  std::size_t start = 0;
  while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start])) != 0) {
    ++start;
  }
  std::size_t end = text.size();
  while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
    --end;
  }
  const std::string token(text.substr(start, end - start));
  if (token.size() != 7 || token.front() != '#') {
    return std::nullopt;
  }
  const auto parse_pair = [&](std::size_t offset) -> std::optional<Uint8> {
    const std::string pair = token.substr(offset, 2);
    char* end_ptr = nullptr;
    const long value = std::strtol(pair.c_str(), &end_ptr, 16);
    if (end_ptr == nullptr || *end_ptr != '\0' || value < 0 || value > 255) {
      return std::nullopt;
    }
    return static_cast<Uint8>(value);
  };

  const auto red = parse_pair(1);
  const auto green = parse_pair(3);
  const auto blue = parse_pair(5);
  if (!red.has_value() || !green.has_value() || !blue.has_value()) {
    return std::nullopt;
  }
  return SDL_Color{*red, *green, *blue, 0xff};
}

std::string FormatProjectColor(SDL_Color color) {
  std::ostringstream stream;
  stream << '#'
         << std::hex << std::setfill('0') << std::nouppercase
         << std::setw(2) << static_cast<int>(color.r)
         << std::setw(2) << static_cast<int>(color.g)
         << std::setw(2) << static_cast<int>(color.b);
  return stream.str();
}

SDL_Color DefaultProjectBaseColor(const std::filesystem::path& project_root) {
  static constexpr std::array<SDL_Color, 8> kPalette = {
      SDL_Color{0x66, 0xa4, 0xff, 0xff},
      SDL_Color{0x5d, 0xd0, 0xb4, 0xff},
      SDL_Color{0xff, 0x9d, 0x5c, 0xff},
      SDL_Color{0xe7, 0x7a, 0x9f, 0xff},
      SDL_Color{0xf0, 0xc3, 0x55, 0xff},
      SDL_Color{0x9c, 0x8d, 0xff, 0xff},
      SDL_Color{0xff, 0x75, 0x75, 0xff},
      SDL_Color{0x7a, 0xd5, 0xff, 0xff},
  };
  const std::uint64_t hash = StablePathHash(project_root.lexically_normal().string());
  return kPalette[static_cast<std::size_t>(hash % kPalette.size())];
}

void ApplyProjectAccent(render::Theme& theme, SDL_Color accent) {
  const auto blend = [&](SDL_Color base, SDL_Color tint, float amount) {
    const float clamped_amount = std::clamp(amount, 0.0f, 1.0f);
    const auto mix_component = [&](Uint8 base_component, Uint8 tint_component) {
      return static_cast<Uint8>(
          std::lround(static_cast<float>(base_component) * (1.0f - clamped_amount) +
                      static_cast<float>(tint_component) * clamped_amount));
    };
    return SDL_Color{
        mix_component(base.r, tint.r),
        mix_component(base.g, tint.g),
        mix_component(base.b, tint.b),
        0xff,
    };
  };

  theme.accent = blend(theme.accent, accent, 0.45f);
  theme.chrome_active = blend(theme.chrome_background, accent, 0.18f);
  theme.row_highlight = blend(theme.editor_background, accent, 0.14f);
  const SDL_Color selection = blend(theme.editor_background, accent, 0.36f);
  theme.selection_fill = SDL_Color{selection.r, selection.g, selection.b, 0xb4};
  const SDL_Color search_match = blend(accent, theme.editor_background, 0.52f);
  theme.search_match = SDL_Color{search_match.r, search_match.g, search_match.b, 0x8f};
  const SDL_Color search_match_active = blend(accent, theme.editor_background, 0.38f);
  theme.search_match_active =
      SDL_Color{search_match_active.r, search_match_active.g, search_match_active.b, 0xc8};
}

}  // namespace microide::workspace
