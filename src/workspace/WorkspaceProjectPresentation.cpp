#include "workspace/WorkspaceProjectPresentation.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <sstream>

#include "platform/AppDirectories.h"
#include "util/Fnv1a.h"
#include "util/PathMatch.h"
#include "util/Hex.h"
#include "util/StringUtil.h"
#include "workspace/persistence/PersistenceService.h"
#include "workspace/WorkspacePathUtils.h"

namespace microide::workspace {

namespace {

std::uint64_t StablePathHash(std::string_view text) { return util::Fnv1aHash(text); }

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

std::string ProjectStateDirectoryName(const std::filesystem::path& project_root) {
  // Memoized because it is a PURE function of `project_root` and an expensive
  // one: `lexically_normal()` alone is ~12 allocations (it walks the path into a
  // component list and rebuilds it), on top of `filename()`, two `.string()`
  // materializations, the sanitize buffer and the two concatenation temporaries.
  //
  // It sits under `ProjectStateDirectory`, which every config/session save
  // resolves from scratch, so a settings change paid the whole thing per save --
  // 288 allocations across the two `settings_change_many_tabs` phases, the
  // largest single site in both (TD-2026-08-30-271).
  //
  // A ring rather than one slot: multi-project sessions alternate between roots
  // (and `ControlChannelService` asks about a second one), so a single slot would
  // thrash. `thread_local` rather than a mutex — the callers are the shell thread
  // and the control thread, and the answer is small enough to compute twice.
  struct Memo {
    std::filesystem::path root;
    std::string name;
  };
  static constexpr std::size_t kMemoSlots = 4;
  thread_local std::array<Memo, kMemoSlots> memo;
  thread_local std::size_t memo_next = 0;
  for (const Memo& entry : memo) {
    if (!entry.name.empty() && entry.root == project_root) {
      return entry.name;
    }
  }

  const std::string label =
      project_root.filename().empty() ? "project" : project_root.filename().string();
  std::string sanitized;
  sanitized.reserve(label.size());
  for (const unsigned char c : label) {
    if (util::IsAsciiAlnum(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '_') {
      sanitized.push_back(static_cast<char>(c));
    } else {
      sanitized.push_back('-');
    }
  }
  if (sanitized.empty()) {
    sanitized = "project";
  }
  std::string name =
      sanitized + "-" + HashToHex(StablePathHash(project_root.lexically_normal().string()));
  memo[memo_next] = Memo{project_root, name};
  memo_next = (memo_next + 1) % kMemoSlots;
  return name;
}

std::filesystem::path ProjectStateDirectory(const std::filesystem::path& project_root) {
  if (project_root.empty()) {
    return {};
  }
  const std::string directory_name = ProjectStateDirectoryName(project_root);
  const std::filesystem::path state_root =
      platform::ResolveAppDirectory(platform::UserDirectoryKind::State, "microide");
  return state_root.empty() ? std::filesystem::path{} : state_root / "projects" / directory_name;
}

std::filesystem::path ProjectConfigStatePath(const std::filesystem::path& project_root) {
  const std::filesystem::path state_dir = ProjectStateDirectory(project_root);
  return state_dir.empty() ? std::filesystem::path{} : state_dir / "config";
}

SDL_Color ResolveProjectTabBadgeColor(const ProjectWorkspaceState& state,
                                      const std::filesystem::path& project_root) {
  if (state.project_base_color.has_value()) {
    return *state.project_base_color;
  }
  return DefaultProjectBaseColor(project_root);
}

void HydrateProjectBaseColorFromConfig(ProjectWorkspaceState& state,
                                       const PersistenceService& persistence_service) {
  if (state.root.empty()) {
    return;
  }

  const std::filesystem::path config_path = ProjectConfigStatePath(state.root);
  PersistedProjectConfigState config{};
  if (!config_path.empty() &&
      persistence_service.LoadProjectConfig(config_path, &config) &&
      config.project_base_color.has_value()) {
    state.project_base_color = config.project_base_color;
    return;
  }

  if (!state.project_base_color.has_value()) {
    state.project_base_color = DefaultProjectBaseColor(state.root);
  }
}

std::string BuildWorkspaceTabDisplayTitle(const std::filesystem::path& path,
                                          std::string_view fallback_title, bool dirty) {
  std::string title = path.filename().string();
  if (title.empty()) {
    title = fallback_title.empty() ? "untitled" : std::string(fallback_title);
  }
  return dirty ? "*" + title : title;
}

std::string BuildWorkspaceTabTooltipLabel(const std::filesystem::path& project_root,
                                          const std::filesystem::path& path,
                                          std::string_view fallback_title) {
  if (path.empty()) {
    return fallback_title.empty() ? "untitled" : std::string(fallback_title);
  }
  return RelativePathLabel(project_root, path);
}

WorkspaceTabTextModel BuildWorkspaceTabTextModel(const std::filesystem::path& project_root,
                                                 const std::filesystem::path& path,
                                                 std::string_view fallback_title,
                                                 bool dirty) {
  return WorkspaceTabTextModel{
      .display_title = BuildWorkspaceTabDisplayTitle(path, fallback_title, dirty),
      .tooltip_label = BuildWorkspaceTabTooltipLabel(project_root, path, fallback_title),
  };
}

std::string BuildEditorBreadcrumbLabel(const std::filesystem::path& project_root,
                                       const std::filesystem::path& path,
                                       bool placeholder) {
  if (path.empty()) {
    return placeholder ? "Welcome" : "Untitled";
  }

  return RelativePathLabel(project_root, path);
}

std::string BuildCompareBreadcrumbLabel(const std::filesystem::path& project_root,
                                        const std::filesystem::path& path,
                                        std::string_view left_label,
                                        std::string_view right_label) {
  const std::string relative_path = RelativePathLabel(project_root, path);
  if (!relative_path.empty()) {
    if (!left_label.empty() || !right_label.empty()) {
      return relative_path + "  |  " + std::string(left_label) + " -> " +
             std::string(right_label);
    }
    return relative_path;
  }
  if (!left_label.empty() || !right_label.empty()) {
    return std::string(left_label) + " -> " + std::string(right_label);
  }
  return "compare";
}

std::string BuildMergeBreadcrumbLabel(const std::filesystem::path& project_root,
                                      const std::filesystem::path& output_path,
                                      std::string_view incoming_label,
                                      std::string_view current_label) {
  const std::string relative_path = RelativePathLabel(project_root, output_path);
  if (!relative_path.empty()) {
    if (!incoming_label.empty() || !current_label.empty()) {
      return relative_path + "  |  " + std::string(incoming_label) + " -> " +
             std::string(current_label);
    }
    return relative_path;
  }
  if (!incoming_label.empty() || !current_label.empty()) {
    return std::string(incoming_label) + " -> " + std::string(current_label);
  }
  return "merge";
}

SDL_Color DefaultProjectBaseColor(const std::filesystem::path& project_root) {
  static constexpr std::array<SDL_Color, 10> kPalette = {
      // Matplotlib qualitative "Accent" base colors.
      SDL_Color{0x7f, 0xc9, 0x7f, 0xff},
      SDL_Color{0xbe, 0xae, 0xd4, 0xff},
      SDL_Color{0xfd, 0xc0, 0x86, 0xff},
      SDL_Color{0xff, 0xff, 0x99, 0xff},
      SDL_Color{0x38, 0x6c, 0xb0, 0xff},
      SDL_Color{0xf0, 0x02, 0x7f, 0xff},
      SDL_Color{0xbf, 0x5b, 0x17, 0xff},
      SDL_Color{0x66, 0x66, 0x66, 0xff},
      // Accent-derived variants so we keep 10 distinct project accents.
      SDL_Color{0x5a, 0x90, 0xca, 0xff},
      SDL_Color{0xd8, 0x4d, 0xa0, 0xff},
  };
  // Catalog roots are normalized on the way in, so `lexically_normal()` here was
  // ~12 allocations producing the string it was handed — once per project tab per
  // painted frame for any project with no explicit base color (TD-2026-08-10-174,
  // same shape as the tooltip path). Only pay it when the text actually needs it.
  const std::uint64_t hash = util::PathTextNeedsNormalizing(project_root.native())
                                 ? StablePathHash(project_root.lexically_normal().string())
                                 : StablePathHash(project_root.native());
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
  theme.chrome_active = blend(theme.chrome_background, accent, 0.14f);
  theme.row_highlight = blend(theme.editor_background, accent, 0.10f);
  const SDL_Color selection = blend(theme.editor_background, accent, 0.30f);
  theme.selection_fill = SDL_Color{selection.r, selection.g, selection.b, 0xa8};
  theme.selection_strong = render::CompositeOver(theme.selection_fill, theme.surface_background);
  const SDL_Color search_match = blend(theme.editor_background, accent, 0.30f);
  theme.search_match = SDL_Color{search_match.r, search_match.g, search_match.b, 0x8f};
  const SDL_Color search_match_active = blend(theme.editor_background, accent, 0.42f);
  theme.search_match_active =
      SDL_Color{search_match_active.r, search_match_active.g, search_match_active.b, 0xa0};
}

}  // namespace microide::workspace
