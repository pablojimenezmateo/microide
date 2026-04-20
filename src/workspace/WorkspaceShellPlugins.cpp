#include "workspace/WorkspaceShell.h"

#include <filesystem>
#include <string_view>

#include "workspace/WorkspaceActionCoordinator.h"
#include "workspace/WorkspaceCommandRegistry.h"

namespace microide::workspace {

WorkspaceShell::WorkspaceShell() {
  plugin_runtime_.SetCallbacks(plugin::PluginHost::Callbacks{
      .is_command_name_available =
          [](std::string_view name) { return FindWorkspaceActionByCommand(name) == nullptr; },
      .open_file =
          [this](const plugin::PluginHost::OpenFileRequest& request) {
            const std::filesystem::path normalized_path = request.path.lexically_normal();
            if (!OpenFileInNewTab(normalized_path)) {
              return false;
            }
            if (request.line > 0) {
              const std::size_t target_line = request.line - 1;
              const std::size_t target_column = request.column > 0 ? request.column - 1 : 0;
              if (editor::TextViewport* viewport = ActiveEditorViewport(); viewport != nullptr) {
                viewport->MoveCursorTo(target_line, target_column);
              }
            }
            return true;
          },
      .active_buffer =
          [this]() -> std::optional<plugin::PluginHost::ActiveBuffer> {
            const editor::TextViewport* viewport = ActiveEditableViewport();
            if (viewport == nullptr || viewport->path().empty()) {
              return std::nullopt;
            }
            return plugin::PluginHost::ActiveBuffer{
                .path = viewport->path().lexically_normal(),
                .line = viewport->cursor_line() + 1,
                .column = viewport->cursor_column() + 1,
            };
          },
      .show_sidebar =
          [this](std::string_view id) {
            return ActionCoordinator(*this).Execute(ActionId::SidebarShow, {std::string(id)},
                                                    ActionSource::Shortcut);
          },
      .publish_diagnostics =
          [this](std::string_view owner,
                 const std::filesystem::path& path,
                 std::vector<editor::Diagnostic> diagnostics) {
            if (diagnostics_store_.ReplaceForOwnerFile(owner, path, std::move(diagnostics))) {
              RefreshProblemsSidebar();
              RequestEditorSurfaceRedraw();
            }
          },
      .clear_file_diagnostics =
          [this](std::string_view owner, const std::filesystem::path& path) {
            if (diagnostics_store_.ClearOwnerFile(owner, path)) {
              RefreshProblemsSidebar();
              RequestEditorSurfaceRedraw();
            }
          },
      .clear_owner_diagnostics =
          [this](std::string_view owner) {
            if (diagnostics_store_.ClearOwner(owner)) {
              RefreshProblemsSidebar();
              RequestEditorSurfaceRedraw();
            }
          },
      .error_sink =
          [this](const std::string& text) {
            plugin_runtime_.AppendError(text);
          },
      .log_sink =
          [this](const std::string& text) {
            plugin_runtime_.AppendLog(text);
          },
  });
}

bool WorkspaceShell::ReloadPluginsForCurrentProject() {
  const bool clean_reload = plugin_runtime_.Reload(project_root_);
  InvalidateRuntimeSyntaxStateCaches();
  RefreshPluginSidebar();
  RefreshProblemsSidebar();
  NotifyPluginsAboutOpenBuffers();
  RequestEditorSurfaceRedraw();
  return clean_reload;
}

bool WorkspaceShell::ReloadPluginsIfPluginAssetsChanged(bool force_check) {
  const bool changed = plugin_runtime_.ConsumeAssetChanges(force_check);
  if (!changed) {
    return false;
  }

  ReloadPluginsForCurrentProject();
  plugin_runtime_.AppendLog("Detected plugin asset changes: " + PluginRuntimeReloadSummary());
  return true;
}

void WorkspaceShell::InvalidateRuntimeSyntaxStateCaches() {
  if (editor::TextViewport* viewport = ActiveEditorViewport(); viewport != nullptr) {
    viewport->InvalidateSyntaxHighlighting();
  }

  for (auto& tab : open_tabs_) {
    if (tab.kind == TabEntry::Kind::Editor && tab.editor_state.has_value()) {
      for (auto& view : tab.editor_state->views) {
        if (!view.needs_restore) {
          view.viewport.InvalidateSyntaxHighlighting();
        }
      }
      continue;
    }

    if (tab.kind == TabEntry::Kind::Compare && tab.compare.has_value()) {
      RefreshCompareTabDerivedState(*tab.compare);
      continue;
    }

    if (tab.kind == TabEntry::Kind::Merge && tab.merge.has_value()) {
      auto& merge_tab = *tab.merge;
      merge_tab.incoming_initial_syntax_state =
          editor::SyntaxHighlighter::InitialState(merge_tab.output_path, merge_tab.model.incoming_lines);
      merge_tab.current_initial_syntax_state =
          editor::SyntaxHighlighter::InitialState(merge_tab.output_path, merge_tab.model.current_lines);
      merge_tab.incoming_current_syntax_state = merge_tab.incoming_initial_syntax_state;
      merge_tab.current_current_syntax_state = merge_tab.current_initial_syntax_state;
      merge_tab.incoming_tokens.assign(merge_tab.model.incoming_lines.size(), {});
      merge_tab.current_tokens.assign(merge_tab.model.current_lines.size(), {});
      merge_tab.incoming_syntax_rows_tokenized = 0;
      merge_tab.current_syntax_rows_tokenized = 0;
      merge_tab.result_viewport.InvalidateSyntaxHighlighting();
    }
  }
}

std::string WorkspaceShell::PluginRuntimeReloadSummary() const {
  return plugin_runtime_.ReloadSummary();
}

void WorkspaceShell::NotifyPluginsAboutOpenBuffers() {
  if (!plugin_runtime_.enabled()) {
    return;
  }
  for (const auto& tab : open_tabs_) {
    if (tab.kind != TabEntry::Kind::Editor || tab.path.empty()) {
      continue;
    }
    NotifyPluginBufferOpen(tab.path);
  }
}

void WorkspaceShell::NotifyPluginBufferOpen(const std::filesystem::path& path) {
  if (!plugin_runtime_.enabled() || path.empty()) {
    return;
  }
  plugin_runtime_.Host().OnBufferOpen(path.lexically_normal());
}

void WorkspaceShell::NotifyPluginBufferSave(const std::filesystem::path& path) {
  if (!plugin_runtime_.enabled() || path.empty()) {
    return;
  }
  plugin_runtime_.Host().OnBufferSave(path.lexically_normal());
}

}  // namespace microide::workspace
