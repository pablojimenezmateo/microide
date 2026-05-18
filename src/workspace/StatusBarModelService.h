#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

#include "editor/TextViewport.h"
#include "workspace/LayoutModeService.h"
#include "workspace/StatusBarService.h"
#include "workspace/WorkspaceProjectState.h"

namespace microide::workspace {

class StatusBarModelService {
 public:
  struct Operations {
    std::function<bool(const std::filesystem::path&)> is_git_repo_valid;
    std::function<void(bool, std::string&, std::string&)> active_lsp_status_strings;
  };

  void Refresh(StatusBarService& status_bar_service,
               const Operations& operations,
               const ProjectWorkspaceState& project_state,
               LayoutMode layout_mode,
               const editor::TextViewport* active_viewport);

 private:
  struct LanguageCache {
    const editor::TextViewport* viewport = nullptr;
    std::uint64_t content_revision = 0;
    std::filesystem::path path;
    std::string filetype;
  };

  struct RepoCache {
    std::filesystem::path project_root;
    bool valid = false;
  };

  struct ProjectSegmentCache {
    std::string branch_label;
    std::string cleanliness;
    std::string text;
    std::string tooltip;
    bool valid = false;
  };

  struct EditorSegmentsCache {
    const editor::TextViewport* viewport = nullptr;
    std::size_t cursor_line = 0;
    std::size_t cursor_column = 0;
    bool soft_tabs = false;
    std::size_t tab_size = 0;
    std::size_t errors = 0;
    std::size_t warnings = 0;
    std::string line_column_text;
    std::string indent_text;
    std::string problems_text;
  };

  LanguageCache language_cache_;
  std::optional<RepoCache> repo_cache_;
  ProjectSegmentCache project_segment_cache_;
  EditorSegmentsCache editor_segments_cache_;
};

}  // namespace microide::workspace
