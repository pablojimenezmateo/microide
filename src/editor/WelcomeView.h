#pragma once

#include <cstdint>
#include <SDL3/SDL.h>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace microide::render {
struct Theme;
}

namespace microide::editor {

struct WelcomeShortcut {
  std::string keys;
  std::string label;
};

struct WelcomeRecent {
  std::string name;
  std::string path_display;
  std::filesystem::path path;
  std::string remote_folder;
};

enum class WelcomeStageState { Done, Active, Attention, Failed };
struct WelcomeRemoteStage {
  std::string text;
  WelcomeStageState state = WelcomeStageState::Active;
  friend bool operator==(const WelcomeRemoteStage&, const WelcomeRemoteStage&) = default;
};
struct WelcomeRemoteStatus {
  std::string heading;
  std::vector<WelcomeRemoteStage> stages;
  std::string detail;
  std::string host;
  bool retry = false;
  bool empty() const { return stages.empty(); }
  friend bool operator==(const WelcomeRemoteStatus&, const WelcomeRemoteStatus&) = default;
};

inline WelcomeStageState WorstRemoteStage(const WelcomeRemoteStatus& status) {
  const auto rank = [](WelcomeStageState state) {
    switch (state) {
      case WelcomeStageState::Done: return 0;
      case WelcomeStageState::Active: return 1;
      case WelcomeStageState::Attention: return 2;
      case WelcomeStageState::Failed: return 3;
    }
    return 3;
  };
  WelcomeStageState worst = status.stages.empty() ? WelcomeStageState::Active : WelcomeStageState::Done;
  for (const WelcomeRemoteStage& stage : status.stages) {
    if (rank(stage.state) > rank(worst)) {
      worst = stage.state;
    }
  }
  return worst;
}

SDL_Color WelcomeStageColor(const render::Theme& theme, WelcomeStageState state);

enum class WelcomeKind { NoProject, ProjectHome };

inline constexpr std::size_t kWelcomeRecentFileLimit = 8;

struct WelcomeViewModel {
  std::uint64_t generation = 0;
  WelcomeKind kind = WelcomeKind::NoProject;
  std::string title;
  std::string subtitle;
  std::string start_heading;
  std::string recents_heading;
  std::string open_folder_label;
  std::string open_remote_folder_label;
  std::string empty_recents_label;
  std::string actions_heading;
  std::string recent_files_heading;
  std::string new_file_label;
  std::string open_file_label;
  std::string find_in_project_label;
  std::string empty_recent_files_label;
  WelcomeRemoteStatus remote;
  std::string retry_label;
  std::string shortcuts_heading;
  std::vector<WelcomeRecent> recent_projects;
  std::vector<WelcomeRecent> recent_files;
  std::vector<WelcomeShortcut> shortcuts;
};

struct WelcomeHitRegion {
  enum class Kind {
    RecentProject,
    OpenFolder,
    OpenRemoteFolder,
    RecentFile,
    NewFile,
    OpenFile,
    FindInProject,
    RemoteRetry
  };
  SDL_FRect rect{};
  Kind kind = Kind::OpenFolder;
  std::size_t recent_index = 0;
};

struct WelcomeLayout {
  SDL_FRect card{};
  SDL_FRect header{};
  SDL_FRect recents_panel{};
  SDL_FRect shortcuts_panel{};
  SDL_FRect open_folder_rect{};
  SDL_FRect open_remote_folder_rect{};
  SDL_FRect remote_block{};
  float recents_rows_top = 0.0f;
  std::optional<float> recents_caption_y;
  bool title_fits = false;
  bool subtitle_fits = false;
  bool panel_headings_fit = false;
  bool empty_recents_fits = false;
  std::vector<WelcomeHitRegion> hit_regions;
};

WelcomeLayout ComputeWelcomeLayout(const SDL_FRect& rect, const WelcomeViewModel& model,
                                   float line_height);

}
