#pragma once

#include <SDL3/SDL.h>

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace microide::editor {

// A single curated keyboard-shortcut row on the welcome surface. Sourced from the
// command registry so the displayed key chord can never drift from the real binding.
struct WelcomeShortcut {
  std::string keys;
  std::string label;
};

// A recent-project entry. `name` is the folder name (primary text), `path_display`
// the muted full path, and `path` the value opened when the row is clicked.
struct WelcomeRecent {
  std::string name;
  std::string path_display;
  std::filesystem::path path;
};

// The welcome surface renders one of two layouts depending on whether a project is
// open. NoProject is the cold-start home (open a folder / recent projects); ProjectHome
// is shown when a project is open but its focused editor group has no tab (recent files
// in this project / new-file / open-file / find-in-project).
enum class WelcomeKind { NoProject, ProjectHome };

// Number of recent files surfaced on the ProjectHome variant. Shared by the renderer and
// the shell hit-test so both agree on how many rows exist.
inline constexpr std::size_t kWelcomeRecentFileLimit = 8;

// View model for the welcome / placeholder surface. All display strings are
// precomputed by RenderViewModelBuilder so the render TU only draws them.
struct WelcomeViewModel {
  WelcomeKind kind = WelcomeKind::NoProject;
  std::string title;
  std::string subtitle;
  // NoProject captions/labels.
  std::string start_heading;
  std::string recents_heading;
  std::string open_folder_label;
  std::string empty_recents_label;
  // ProjectHome captions/labels.
  std::string actions_heading;
  std::string recent_files_heading;
  std::string new_file_label;
  std::string open_file_label;
  std::string find_in_project_label;
  std::string empty_recent_files_label;
  // Shared right column.
  std::string shortcuts_heading;
  std::vector<WelcomeRecent> recent_projects;  // NoProject
  std::vector<WelcomeRecent> recent_files;      // ProjectHome
  std::vector<WelcomeShortcut> shortcuts;
};

// An interactive region on the welcome surface, produced by ComputeWelcomeLayout and
// shared by the renderer (to draw it) and the shell (to hit-test clicks / hover).
struct WelcomeHitRegion {
  enum class Kind { RecentProject, OpenFolder, RecentFile, NewFile, OpenFile, FindInProject };
  SDL_FRect rect{};
  Kind kind = Kind::OpenFolder;
  std::size_t recent_index = 0;  // valid when kind == RecentProject or RecentFile
};

struct WelcomeLayout {
  SDL_FRect card{};
  SDL_FRect header{};
  SDL_FRect recents_panel{};
  SDL_FRect shortcuts_panel{};
  SDL_FRect open_folder_rect{};
  // Y at which recent-project rows (or the empty-state caption) begin. The
  // renderer draws the empty state here so it can never collide with the
  // open-folder button, which now owns a dedicated slot above this point.
  float recents_rows_top = 0.0f;
  std::vector<WelcomeHitRegion> hit_regions;
};

// Pure geometry: lay out the welcome card, its two panels, the recent-project rows,
// and the open-folder affordance for a given editor rect + model. Deterministic so
// the renderer and the shell hit-test stay in lock-step.
WelcomeLayout ComputeWelcomeLayout(const SDL_FRect& rect, const WelcomeViewModel& model,
                                   float line_height);

}  // namespace microide::editor
