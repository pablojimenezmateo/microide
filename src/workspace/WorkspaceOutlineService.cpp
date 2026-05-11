#include "workspace/WorkspaceOutlineService.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iomanip>
#include <regex>
#include <sstream>
#include <utility>

#include "editor/RuntimeSyntaxRegistry.h"
#include "workspace/WorkspaceLanguageContract.h"
#include "workspace/WorkspaceLspClient.h"
#include "workspace/WorkspaceLspManager.h"
#include "workspace/WorkspaceProjectState.h"
#include "workspace/WorkspaceTabState.h"

namespace microide::workspace {

namespace {

bool IsUnreservedUriByte(unsigned char ch) {
  return std::isalnum(ch) != 0 || ch == '-' || ch == '_' || ch == '.' || ch == '~' || ch == '/';
}

std::string FileUriForPath(const std::filesystem::path& path) {
  const std::string raw = path.lexically_normal().generic_string();
  std::ostringstream encoded;
  encoded << "file://";
  for (unsigned char ch : raw) {
    if (IsUnreservedUriByte(ch)) {
      encoded << static_cast<char>(ch);
      continue;
    }
    encoded << '%' << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(ch) << std::nouppercase << std::dec;
  }
  return encoded.str();
}

editor::TextViewport* FindEditorView(TabEntry::EditorTabState& editor_tab, std::size_t leaf_id) {
  auto it = std::find_if(editor_tab.views.begin(), editor_tab.views.end(),
                         [&](const TabEntry::EditorTabState::EditorViewState& v) {
                           return v.leaf_id == leaf_id;
                         });
  return it == editor_tab.views.end() ? nullptr : &it->viewport;
}

editor::TextViewport* ActiveEditorBuffer(ProjectWorkspaceState& project) {
  if (project.active_tab_index >= project.open_tabs.size()) {
    return &project.welcome_surface.viewport;
  }
  TabEntry& tab = project.open_tabs[project.active_tab_index];
  if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
    return nullptr;
  }
  TabEntry::EditorTabState& es = tab.editor_state.value();
  if (es.views.empty()) {
    return nullptr;
  }
  if (editor::TextViewport* vp = FindEditorView(es, es.active_leaf_id); vp != nullptr) {
    return vp;
  }
  return &es.views.front().viewport;
}

bool SettingTruthy(const WorkspaceOutlineService::SettingGetter& get_setting,
                   std::string_view id,
                   bool default_true) {
  if (!get_setting) {
    return default_true;
  }
  const auto v = get_setting(id);
  if (!v.has_value()) {
    return default_true;
  }
  const std::string& s = *v;
  return !(s == "false" || s == "0" || s == "off");
}

OutlineSymbolNode ConvertTree(const LspClient::DocumentSymbol& sym) {
  OutlineSymbolNode node;
  node.name = sym.name;
  node.kind = sym.kind;
  node.selection_line =
      static_cast<std::size_t>(std::max(0, sym.selection_range.start.line));
  node.selection_column =
      static_cast<std::size_t>(std::max(0, sym.selection_range.start.character));
  node.children.reserve(sym.children.size());
  for (const auto& ch : sym.children) {
    node.children.push_back(ConvertTree(ch));
  }
  return node;
}

void BuildRegexOutline(const LanguageContract& contract,
                       const editor::TextViewport& viewport,
                       OutlineSidebarState& out) {
  out.roots.clear();
  if (contract.outline_regex_patterns.empty()) {
    return;
  }
  thread_local std::vector<std::regex> scratch_regex;
  scratch_regex.clear();
  scratch_regex.reserve(contract.outline_regex_patterns.size());
  for (const std::string& pattern : contract.outline_regex_patterns) {
    try {
      scratch_regex.emplace_back(pattern, std::regex_constants::ECMAScript);
    } catch (...) {
      continue;
    }
  }
  const auto& lines = viewport.lines();
  for (std::size_t line_idx = 0; line_idx < lines.size(); ++line_idx) {
    const std::string& line = lines[line_idx];
    for (const std::regex& re : scratch_regex) {
      std::smatch match;
      if (!std::regex_search(line, match, re) || match.size() <= 1) {
        continue;
      }
      OutlineSymbolNode node;
      node.name = match[1].str();
      node.kind = 1;
      node.selection_line = line_idx;
      if (match[1].matched) {
        node.selection_column = static_cast<std::size_t>(match.position(1));
      }
      out.roots.push_back(std::move(node));
      break;
    }
  }
}

void ApplyRegexFallback(ProjectWorkspaceState& project,
                        editor::TextViewport& viewport,
                        const WorkspaceLanguageContract& contracts) {
  const std::string language_id =
      editor::runtime_syntax::DetectFiletype(viewport.path(), viewport.lines());
  const LanguageContract* contract = contracts.Find(language_id);
  if (contract == nullptr) {
    project.sidebar.outline.roots.clear();
    project.sidebar.outline.from_fallback = true;
    project.sidebar.outline.indexing = false;
    return;
  }
  BuildRegexOutline(*contract, viewport, project.sidebar.outline);
  project.sidebar.outline.from_fallback = true;
  project.sidebar.outline.indexing = false;
}

}  // namespace

void WorkspaceOutlineService::ScheduleDebouncedRefresh() {
  debounce_pending_ = true;
  debounce_deadline_ms_ = NowMs() + 150;
}

uint32_t WorkspaceOutlineService::NowMs() const {
  if (fixed_clock_ms_.has_value()) {
    return *fixed_clock_ms_;
  }
  return SDL_GetTicks();
}

void WorkspaceOutlineService::ResetCountsForTesting() {
  refresh_count_for_testing_ = 0;
  lsp_request_count_for_testing_ = 0;
}

void WorkspaceOutlineService::Poll(uint32_t time_ms,
                                   bool outline_setting_enabled,
                                   ProjectWorkspaceState& project,
                                   LspManager& lsp,
                                   const WorkspaceLanguageContract& contracts,
                                   const SettingGetter& get_setting) {
  (void)time_ms;
  if (!outline_setting_enabled) {
    debounce_pending_ = false;
    debounce_deadline_ms_.reset();
    project.sidebar.outline.Clear();
    tracked_tab_index_ = static_cast<std::size_t>(-1);
    tracked_doc_key_.clear();
    return;
  }

  editor::TextViewport* viewport = ActiveEditorBuffer(project);
  if (viewport == nullptr || viewport->path().empty()) {
    project.sidebar.outline.Clear();
    tracked_tab_index_ = static_cast<std::size_t>(-1);
    tracked_doc_key_.clear();
    return;
  }

  const std::size_t tab_index = project.active_tab_index;
  const std::string doc_key = viewport->path().lexically_normal().generic_string();
  const bool context_changed =
      tab_index != tracked_tab_index_ || doc_key != tracked_doc_key_;
  if (context_changed) {
    tracked_tab_index_ = tab_index;
    tracked_doc_key_ = doc_key;
    debounce_pending_ = false;
    debounce_deadline_ms_.reset();
    RefreshOutlineForActiveEditor(true, project, lsp, contracts, get_setting);
    return;
  }

  if (debounce_pending_ && debounce_deadline_ms_.has_value() && NowMs() >= *debounce_deadline_ms_) {
    debounce_pending_ = false;
    debounce_deadline_ms_.reset();
    RefreshOutlineForActiveEditor(true, project, lsp, contracts, get_setting);
  }
}

void WorkspaceOutlineService::RefreshOutlineForActiveEditor(
    bool /*prefer_immediate_async*/,
    ProjectWorkspaceState& project,
    LspManager& lsp,
    const WorkspaceLanguageContract& contracts,
    const SettingGetter& get_setting) {
  if (!SettingTruthy(get_setting, "editor.outline.enabled", true)) {
    return;
  }
  editor::TextViewport* viewport = ActiveEditorBuffer(project);
  if (viewport == nullptr || viewport->path().empty()) {
    project.sidebar.outline.Clear();
    return;
  }

  ++refresh_count_for_testing_;

  const std::string language_id =
      editor::runtime_syntax::DetectFiletype(viewport->path(), viewport->lines());
  const std::string uri = FileUriForPath(viewport->path());
  const std::string expected_doc_key =
      viewport->path().lexically_normal().generic_string();
  LspClient* client = lsp.FindStartedServer(language_id);

  if (client == nullptr || !client->IsInitialized() || !client->HasOpenDocument(uri)) {
    ApplyRegexFallback(project, *viewport, contracts);
    return;
  }

  ++lsp_callback_token_;
  const std::uint64_t callback_token = lsp_callback_token_;
  ++lsp_request_count_for_testing_;

  project.sidebar.outline.indexing = true;
  project.sidebar.outline.from_fallback = false;

  client->RequestDocumentSymbolAsync(
      uri, [this, callback_token, expected_key = expected_doc_key, &project, &contracts](
               std::optional<std::vector<LspClient::DocumentSymbol>> syms) {
    if (callback_token != lsp_callback_token_) {
      return;
    }
    if (tracked_doc_key_ != expected_key) {
      return;
    }
    project.sidebar.outline.indexing = false;
    if (!syms.has_value() || syms->empty()) {
      editor::TextViewport* vp = ActiveEditorBuffer(project);
      if (vp == nullptr || vp->path().lexically_normal().generic_string() != expected_key) {
        return;
      }
      ApplyRegexFallback(project, *vp, contracts);
      return;
    }
    project.sidebar.outline.from_fallback = false;
    project.sidebar.outline.roots.clear();
    project.sidebar.outline.collapsed_paths.clear();
    project.sidebar.outline.roots.reserve(syms->size());
    for (const auto& root : *syms) {
      project.sidebar.outline.roots.push_back(ConvertTree(root));
    }
  });
}

}  // namespace microide::workspace
