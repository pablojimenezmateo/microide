#include "workspace/actions/WorkspaceActionServices.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "editor/TextViewport.h"
#include "util/PerformanceTrace.h"
#include "util/StringUtil.h"
#include "workspace/SettingFlags.h"
#include "workspace/WorkspaceCommandParsing.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/coordinators/WorkspaceCommandLineCoordinator.h"
#include "workspace/coordinators/WorkspaceMenuCoordinator.h"
#include "workspace/persistence/WorkspacePersistenceCoordinator.h"
#include "workspace/shell/WorkspaceShell.h"
#include "workspace/coordinators/WorkspaceTextInputCoordinator.h"

namespace microide::workspace {

ClipboardExportResult ClampClipboardExport(std::string_view text, std::size_t budget) {
  if (text.size() <= budget) {
    return {std::string(text), false};
  }
  const std::size_t fit = util::Utf8ByteBudgetPrefixLength(text, budget);
  ClipboardExportResult result;
  result.text.reserve(fit + 24);
  result.text.assign(text.substr(0, fit));
  result.text += "\n…[clipboard truncated]";
  result.truncated = true;
  return result;
}

WorkspaceActionContext::WorkspaceActionContext(ProjectCatalogState& project_catalog,
                                               ProjectWorkspaceState& current_project_state,
                                               float& ui_scale,
                                               Operations operations)
    : project_catalog_(project_catalog),
      state_(current_project_state),
      ui_scale_(ui_scale),
      operations_(std::move(operations)) {}

void WorkspaceActionContext::PrepareForAction(ActionSource source) {
  if (source != ActionSource::ContextMenu) {
    operations_.close_tree_context_menu();
  }
}

bool WorkspaceActionContext::RejectAction(ActionSource source, std::string feedback) {
  return operations_.reject_action(source, std::move(feedback));
}

SidebarViewRequest WorkspaceActionContext::ParseSidebarViewRequest(
    const std::vector<std::string>& args) const {
  return operations_.parse_sidebar_view_request(args);
}

bool WorkspaceActionContext::HasProjectRoot() const {
  return !state_.root.empty();
}

bool WorkspaceActionContext::HasActiveProject() const {
  return !project_catalog_.entries.empty() && HasProjectRoot();
}

std::size_t WorkspaceActionContext::ProjectCount() const {
  return project_catalog_.entries.size();
}

std::size_t WorkspaceActionContext::ActiveProjectIndex() const {
  return project_catalog_.active_index;
}

std::filesystem::path WorkspaceActionContext::ProjectRoot() const {
  return state_.root;
}

bool WorkspaceActionContext::OpenProject(const std::filesystem::path& project_root,
                                         bool restore_persistence,
                                         bool log_feedback) {
  return operations_.open_project(project_root, restore_persistence, log_feedback);
}

void WorkspaceActionContext::RequestCloseProject(std::size_t index) {
  operations_.request_close_project(index);
}

bool WorkspaceActionContext::SwitchProject(std::size_t index, bool log_feedback) {
  return operations_.switch_project(index, log_feedback);
}

ProjectOpenPickerResult WorkspaceActionContext::OpenNativeProjectPicker() {
  return operations_.open_native_project_picker();
}

ProjectOpenPickerResult WorkspaceActionContext::OpenNativeFilePicker() {
  return operations_.open_native_file_picker();
}

bool WorkspaceActionContext::SidebarVisible() const {
  return state_.sidebar.visible;
}

std::string_view WorkspaceActionContext::SidebarViewId() const {
  return state_.sidebar.view_id;
}

SidebarMode WorkspaceActionContext::ActiveSidebarMode() const {
  return operations_.active_sidebar_mode();
}

void WorkspaceActionContext::ShowSidebarSurface() {
  state_.sidebar.visible = true;
  state_.surface.focus = FocusTarget::Sidebar;
}

void WorkspaceActionContext::ToggleSidebar() {
  operations_.toggle_sidebar();
}

void WorkspaceActionContext::CloseSidebar() {
  operations_.close_sidebar();
}

bool WorkspaceActionContext::ShowSidebarView(const SidebarViewInfo& view,
                                             const std::filesystem::path& root,
                                             const std::string& query) {
  switch (view.mode) {
    case SidebarMode::Tree:
      operations_.show_tree_sidebar(root);
      return true;
    case SidebarMode::Search:
      operations_.show_search_sidebar(query, false);
      return true;
    case SidebarMode::Problems:
      operations_.show_problems_sidebar();
      return true;
    case SidebarMode::Git:
      operations_.show_git_sidebar();
      return true;
    case SidebarMode::Tests:
      operations_.show_tests_sidebar();
      return true;
    case SidebarMode::Outline:
      operations_.show_outline_sidebar();
      return true;
    case SidebarMode::Plugin:
      return operations_.show_plugin_sidebar(view.id, false);
    case SidebarMode::None:
      return false;
  }
  return false;
}

bool WorkspaceActionContext::ToggleSidebarView(const SidebarViewInfo& view,
                                               const std::filesystem::path& root,
                                               const std::string& query) {
  const bool same_view = state_.sidebar.visible && state_.sidebar.view_id == view.id;
  switch (view.mode) {
    case SidebarMode::Tree:
    case SidebarMode::Problems:
    case SidebarMode::Git:
    case SidebarMode::Tests:
    case SidebarMode::Outline:
    case SidebarMode::Plugin:
      if (same_view) {
        operations_.close_sidebar();
        return true;
      }
      return ShowSidebarView(view, root, query);
    case SidebarMode::Search:
      if (same_view && !state_.sidebar.temporary) {
        operations_.close_sidebar();
        return true;
      }
      return ShowSidebarView(view, root, query);
    case SidebarMode::None:
      return false;
  }
  return false;
}

float WorkspaceActionContext::CurrentWindowWidth() const {
  if (const std::optional<SDL_FRect> rect = operations_.current_window_rect(); rect.has_value()) {
    return rect->w;
  }
  return 1.0f;
}

void WorkspaceActionContext::SetSidebarWidth(float width) {
  state_.sidebar.width = ClampSidebarWidth(width, std::max(1.0f, CurrentWindowWidth()));
}

void WorkspaceActionContext::RefreshProjectFiles() {
  operations_.refresh_project_files();
}

void WorkspaceActionContext::ReloadCleanOpenBuffersFromDisk() {
  operations_.reload_clean_open_buffers_from_disk();
}

std::string WorkspaceActionContext::DispatchGitOperation(const ActionId id,
                                                         const std::vector<std::string>& args) {
  if (operations_.dispatch_git_operation == nullptr) {
    return "Git operations are unavailable";
  }
  return operations_.dispatch_git_operation(id, args);
}

std::filesystem::path WorkspaceActionContext::TreeMutationBasePath(ActionSource source) const {
  return operations_.tree_mutation_base_path(source);
}

std::filesystem::path WorkspaceActionContext::ResolveTreeActionPath(ActionSource source) const {
  return operations_.resolve_tree_action_path(source);
}

std::filesystem::path WorkspaceActionContext::RowContextMenuPath() const {
  return operations_.row_context_menu_path ? operations_.row_context_menu_path()
                                           : std::filesystem::path{};
}

std::filesystem::path WorkspaceActionContext::ActiveTabPath() const {
  return operations_.active_tab_path();
}

void WorkspaceActionContext::OpenCreatePathPrompt(bool directory,
                                                  const std::filesystem::path& base_path) {
  operations_.open_prompt_surface(directory ? PromptSurfaceState::Action::CreateDirectory
                                            : PromptSurfaceState::Action::CreateFile,
                                  PromptSurfaceState::Kind::TextInput, base_path, {});
}

void WorkspaceActionContext::OpenRenamePathPrompt(const std::filesystem::path& path) {
  operations_.open_prompt_surface(PromptSurfaceState::Action::RenamePath,
                                  PromptSurfaceState::Kind::TextInput, path,
                                  path.filename().string());
}

void WorkspaceActionContext::OpenDeletePathPrompt(const std::filesystem::path& path) {
  operations_.open_prompt_surface(PromptSurfaceState::Action::DeletePath,
                                  PromptSurfaceState::Kind::Confirm, path, {});
}

bool WorkspaceActionContext::WriteClipboardText(std::string_view text) const {
  // Zero-copy for the common in-budget case; only an over-budget export materializes a
  // clamped copy (TD-2026-07-17A-119).
  if (text.size() <= kMaxClipboardExportBytes) {
    return operations_.write_clipboard_text(text);
  }
  const ClipboardExportResult clamped = ClampClipboardExport(text);
  if (operations_.notify) {
    operations_.notify(NotificationService::Tone::Warning,
                       "Copied text truncated (too large for clipboard)");
  }
  return operations_.write_clipboard_text(clamped.text);
}

bool WorkspaceActionContext::WritePrimarySelectionText(std::string_view text) const {
  if (text.size() <= kMaxClipboardExportBytes) {
    return operations_.write_primary_selection_text(text);
  }
  const ClipboardExportResult clamped = ClampClipboardExport(text);
  return operations_.write_primary_selection_text(clamped.text);
}

bool WorkspaceActionContext::RevealPathInFileExplorer(
    const std::filesystem::path& directory) const {
  return operations_.reveal_path_in_file_explorer(directory);
}

bool WorkspaceActionContext::RevealPathInTree(const std::filesystem::path& path) const {
  return operations_.reveal_path_in_tree(path);
}

void WorkspaceActionContext::OpenTerminal(std::string command) {
  operations_.open_terminal(std::move(command));
}

bool WorkspaceActionContext::CloseActiveTerminal() {
  return operations_.close_active_terminal && operations_.close_active_terminal();
}

bool WorkspaceActionContext::OpenTerminalFind(std::string query) {
  return operations_.open_terminal_find(std::move(query));
}

void WorkspaceActionContext::ShowFileFinderWithQuery(std::string query) {
  state_.file_finder.SetIndex(&state_.file_index);
  state_.file_finder.SetQuery(std::move(query));
  operations_.show_overlay(OverlayMode::FileFinder);
}

void WorkspaceActionContext::ShowFileFinder() {
  operations_.show_overlay(OverlayMode::FileFinder);
  state_.file_finder.SetIndex(&state_.file_index);
  state_.file_finder.SetQuery("");
}

bool WorkspaceActionContext::OverlayVisible() const {
  return state_.overlay.visible;
}

void WorkspaceActionContext::DismissOverlay() {
  operations_.dismiss_overlay();
}

void WorkspaceActionContext::OpenSettingsOverlay() {
  operations_.open_settings_overlay();
}

void WorkspaceActionContext::OpenHelpAboutOverlay() {
  operations_.open_help_about_overlay();
}

void WorkspaceActionContext::ToggleStatusBar() {
  operations_.toggle_status_bar();
}

void WorkspaceActionContext::ToggleLayoutMode() {
  operations_.toggle_layout_mode();
}

void WorkspaceActionContext::ShowProjectSearchSidebar(std::string query) {
  operations_.show_search_sidebar(std::move(query), true);
}

bool WorkspaceActionContext::ShowCompletionOverlay(std::string* error_message) {
  return operations_.show_completion_overlay(error_message);
}

bool WorkspaceActionContext::ShowInsertSnippetOverlay(std::string* error_message) {
  return operations_.show_insert_snippet_overlay(error_message);
}

bool WorkspaceActionContext::ShowCodeActionsOverlay(std::string* error_message) {
  return operations_.show_code_actions_overlay(error_message);
}

bool WorkspaceActionContext::GoToLspDefinition(std::string* error_message) {
  return operations_.go_to_lsp_definition(error_message);
}

bool WorkspaceActionContext::GoToLspTypeDefinition(std::string* error_message) {
  return operations_.go_to_lsp_type_definition(error_message);
}

bool WorkspaceActionContext::GoToLspImplementation(std::string* error_message) {
  return operations_.go_to_lsp_implementation(error_message);
}

bool WorkspaceActionContext::GoToLspDeclaration(std::string* error_message) {
  return operations_.go_to_lsp_declaration(error_message);
}

bool WorkspaceActionContext::FormatActiveDocument(std::string* error_message) {
  return operations_.format_active_document(error_message);
}

bool WorkspaceActionContext::RenameSymbol(const std::string& new_name, std::string* error_message) {
  return operations_.rename_symbol(new_name, error_message);
}

bool WorkspaceActionContext::FindLspReferences(std::string* error_message) {
  return operations_.find_lsp_references(error_message);
}

bool WorkspaceActionContext::ShowWorkspaceSymbols(const std::string& query,
                                                  std::string* error_message) {
  return operations_.show_workspace_symbols(query, error_message);
}

bool WorkspaceActionContext::ShowCallHierarchy(bool incoming, std::string* error_message) {
  return operations_.show_call_hierarchy(incoming, error_message);
}

bool WorkspaceActionContext::ShowSignatureHelp(std::string* error_message) {
  return operations_.show_signature_help(error_message);
}

bool WorkspaceActionContext::DiscoverTestsForActiveBuffer(std::string* error_message) {
  return operations_.discover_tests_for_active_buffer(error_message);
}

bool WorkspaceActionContext::RunTests(const std::vector<std::string>& test_ids,
                                      std::string* error_message) {
  return operations_.run_tests(test_ids, error_message);
}

bool WorkspaceActionContext::RunAllDiscoveredTests(std::string* error_message) {
  return operations_.run_all_discovered_tests(error_message);
}

bool WorkspaceActionContext::ActiveTabIsCompare() const {
  return state_.focused_group().active_tab_index < state_.focused_group().open_tabs.size() &&
         state_.focused_group().open_tabs[state_.focused_group().active_tab_index].kind == TabEntry::Kind::Compare;
}

bool WorkspaceActionContext::ActiveTabIsMerge() const {
  return state_.focused_group().active_tab_index < state_.focused_group().open_tabs.size() &&
         state_.focused_group().open_tabs[state_.focused_group().active_tab_index].kind == TabEntry::Kind::Merge;
}

void WorkspaceActionContext::OpenBufferSearch(std::string query) {
  operations_.open_buffer_search();
  // Only an explicit query (e.g. a `:search foo` command) overrides the term the
  // widget opened with. The bare Ctrl+F shortcut passes no query and must keep
  // open_buffer_search's behaviour (seed from selection, else reuse the last term).
  if (!query.empty()) {
    state_.overlay.workflow.buffer_search.query.SetText(std::move(query));
    operations_.refresh_buffer_search();
  }
}

void WorkspaceActionContext::OpenBufferReplace() {
  operations_.open_buffer_replace();
}

std::filesystem::path WorkspaceActionContext::ResolveComparePath(
    const std::filesystem::path& requested_path,
    ActionSource source) const {
  if (!requested_path.empty()) {
    return requested_path;
  }
  if (source == ActionSource::ContextMenu) {
    return operations_.resolve_tree_action_path(source);
  }
  if (const editor::TextViewport* viewport = operations_.active_editor_viewport();
      viewport != nullptr && !viewport->path().empty()) {
    return viewport->path().lexically_normal();
  }
  if (state_.sidebar.visible && operations_.active_sidebar_mode() == SidebarMode::Tree) {
    const auto& entries = state_.directory_tree.entries();
    if (state_.directory_tree.selected_index() < entries.size() &&
        !entries[state_.directory_tree.selected_index()].is_directory) {
      return entries[state_.directory_tree.selected_index()].path.lexically_normal();
    }
  }
  return {};
}

void WorkspaceActionContext::OpenComparePickerForPath(const std::filesystem::path& path,
                                                      const std::string& commit_spec) {
  operations_.open_compare_picker_for_path(path, commit_spec);
}

void WorkspaceActionContext::OpenHeadComparison(const std::filesystem::path& path) {
  state_.overlay.workflow.compare_picker.path = path.lexically_normal();
  operations_.open_comparison(project::GitCommitEntry{
      .hash = "HEAD",
      .short_hash = "HEAD",
      .subject = "HEAD",
      .author = {},
      .relative_date = {},
  });
}

std::optional<CompareInput> WorkspaceActionContext::ResolveCurrentCompareInput(
    ActionSource source, bool* from_file, std::string* error) const {
  const auto fail = [&](std::string message) -> std::optional<CompareInput> {
    if (error != nullptr) {
      *error = std::move(message);
    }
    return std::nullopt;
  };
  const auto read_file = [&](const std::filesystem::path& path) -> std::optional<CompareInput> {
    if (from_file != nullptr) {
      *from_file = true;
    }
    auto input = ReadFileCompareInput(path, /*editable=*/true);
    if (!input.has_value()) {
      return fail("Cannot compare (unreadable or binary file): " + path.string());
    }
    return input;
  };

  // A context-menu invocation acts on the file targeted in the tree.
  if (source == ActionSource::ContextMenu) {
    const std::filesystem::path path =
        operations_.resolve_tree_action_path ? operations_.resolve_tree_action_path(source)
                                              : std::filesystem::path{};
    if (path.empty()) {
      return fail("No file selected to compare");
    }
    return read_file(path);
  }

  // Otherwise prefer the active editor buffer — its live, possibly-unsaved text.
  if (operations_.active_editor_viewport != nullptr) {
    if (editor::TextViewport* viewport = operations_.active_editor_viewport(); viewport != nullptr) {
      const std::filesystem::path path = viewport->path().lexically_normal();
      CompareInput input;
      input.content = util::SerializeLinesStreaming(editor::LineSpan(viewport->lines()),
                                    viewport->line_ending());
      input.path = path;
      input.label = path.empty() ? std::string("Untitled") : path.filename().string();
      input.editable = !path.empty();
      if (from_file != nullptr) {
        *from_file = false;
      }
      return input;
    }
  }

  // Fall back to a file resolved from the tree selection.
  const std::filesystem::path path = ResolveComparePath({}, source);
  if (!path.empty()) {
    return read_file(path);
  }
  return fail("No file or buffer selected to compare");
}

std::string WorkspaceActionContext::CompareFiles(const std::filesystem::path& left_path,
                                                 const std::filesystem::path& right_path) {
  auto left = ReadFileCompareInput(left_path, /*editable=*/false);
  if (!left.has_value()) {
    return "Cannot read left file: " + left_path.string();
  }
  auto right = ReadFileCompareInput(right_path, /*editable=*/true);
  if (!right.has_value()) {
    return "Cannot read right file: " + right_path.string();
  }
  if (!operations_.open_plain_comparison ||
      !operations_.open_plain_comparison(std::move(*left), std::move(*right))) {
    return "Failed to open comparison";
  }
  return {};
}

std::string WorkspaceActionContext::SelectForCompare(ActionSource source) {
  bool from_file = false;
  std::string error;
  auto input = ResolveCurrentCompareInput(source, &from_file, &error);
  if (!input.has_value()) {
    return error;
  }
  const std::string label = input->label;
  state_.compare_selection = std::move(*input);
  state_.compare_selection_from_file = from_file;
  if (operations_.notify) {
    operations_.notify(NotificationService::Tone::Info, "Selected '" + label + "' for compare");
  }
  return {};
}

std::string WorkspaceActionContext::CompareWithSelected(ActionSource source) {
  if (!state_.compare_selection.has_value()) {
    return "Nothing selected — use 'Select for Compare' first";
  }
  CompareInput stashed = *state_.compare_selection;
  // Re-read a file source so the compare reflects edits made since it was
  // selected; keep the captured snapshot if the file has since vanished.
  if (state_.compare_selection_from_file && !stashed.path.empty()) {
    if (auto fresh = ReadFileCompareInput(stashed.path, stashed.editable); fresh.has_value()) {
      fresh->label = stashed.label;
      stashed = std::move(*fresh);
    }
  }
  bool from_file = false;
  std::string error;
  auto current = ResolveCurrentCompareInput(source, &from_file, &error);
  if (!current.has_value()) {
    return error;
  }
  if (!operations_.open_plain_comparison ||
      !operations_.open_plain_comparison(std::move(stashed), std::move(*current))) {
    return "Failed to open comparison";
  }
  return {};
}

std::string WorkspaceActionContext::CompareWithClipboard(ActionSource source) {
  bool from_file = false;
  std::string error;
  auto current = ResolveCurrentCompareInput(source, &from_file, &error);
  if (!current.has_value()) {
    return error;
  }
  std::optional<std::string> clip =
      operations_.read_clipboard_text ? operations_.read_clipboard_text() : std::nullopt;
  if (!clip.has_value() || clip->empty()) {
    return "Clipboard is empty";
  }
  CompareInput clipboard_input{
      .content = std::move(*clip),
      .label = "Clipboard",
      .path = {},
      .editable = false,
  };
  if (!operations_.open_plain_comparison ||
      !operations_.open_plain_comparison(std::move(clipboard_input), std::move(*current))) {
    return "Failed to open comparison";
  }
  return {};
}

void WorkspaceActionContext::MarkBranchFileReviewed() {
  if (operations_.mark_branch_file_reviewed != nullptr) {
    operations_.mark_branch_file_reviewed();
  }
}

void WorkspaceActionContext::UnmarkBranchFileReviewed() {
  if (operations_.unmark_branch_file_reviewed != nullptr) {
    operations_.unmark_branch_file_reviewed();
  }
}

void WorkspaceActionContext::MarkBranchHunkReviewed() {
  if (operations_.mark_branch_hunk_reviewed != nullptr) {
    operations_.mark_branch_hunk_reviewed();
  }
}

void WorkspaceActionContext::UnmarkBranchHunkReviewed() {
  if (operations_.unmark_branch_hunk_reviewed != nullptr) {
    operations_.unmark_branch_hunk_reviewed();
  }
}

void WorkspaceActionContext::ClearBranchReviewState() {
  if (operations_.clear_branch_review_state != nullptr) {
    operations_.clear_branch_review_state();
  }
}

void WorkspaceActionContext::EditBranchReviewNote(std::string note_text) {
  if (operations_.edit_branch_review_note != nullptr) {
    operations_.edit_branch_review_note(std::move(note_text));
  }
}

void WorkspaceActionContext::OpenMergeEditor(const std::filesystem::path& base_path,
                                             const std::filesystem::path& incoming_path,
                                             const std::filesystem::path& current_path,
                                             const std::filesystem::path& output_path) {
  operations_.open_merge_editor(base_path, incoming_path, current_path, output_path);
}

namespace {

ReviewOpenOutcome FinishReviewOutcome(ProjectWorkspaceState& state,
                                      const WorkspaceActionContext::Operations& operations,
                                      ReviewOpenOutcome outcome) {
  // Always surface the summary as command feedback so the control channel
  // reports it (as feedback on success, as error on failure).
  state.panel.feedback.text = outcome.message;
  if (operations.notify && !outcome.message.empty()) {
    operations.notify(outcome.ok ? NotificationService::Tone::Info
                                 : NotificationService::Tone::Warning,
                      outcome.message);
  }
  return outcome;
}

}  // namespace

ReviewOpenOutcome WorkspaceActionContext::ReviewConflicts() {
  if (!operations_.open_conflict_review) {
    return {false, "review-conflicts: unavailable"};
  }
  return FinishReviewOutcome(state_, operations_, operations_.open_conflict_review());
}

ReviewOpenOutcome WorkspaceActionContext::ReviewBranch(const std::string& ref) {
  if (!operations_.open_branch_review) {
    return {false, "review-branch: unavailable"};
  }
  return FinishReviewOutcome(state_, operations_, operations_.open_branch_review(ref));
}

ReviewOpenOutcome WorkspaceActionContext::ReviewCommit(const std::string& ref) {
  if (!operations_.open_commit_review) {
    return {false, "review-commit: unavailable"};
  }
  return FinishReviewOutcome(state_, operations_, operations_.open_commit_review(ref));
}

bool WorkspaceActionContext::OpenPath(const std::filesystem::path& path,
                                      std::string* /*error_message*/) {
  operations_.open_file(path);
  return true;
}

bool WorkspaceActionContext::OpenPathInNewTab(const std::filesystem::path& path) {
  return operations_.open_file_in_new_tab(path);
}

bool WorkspaceActionContext::OpenUntitledTab() {
  return operations_.open_untitled_tab();
}

std::optional<std::size_t> WorkspaceActionContext::FindTabIndexBySpecifier(
    std::string_view specifier,
    std::string* error_message) const {
  return operations_.find_tab_index_by_specifier(specifier, error_message);
}

void WorkspaceActionContext::ActivateTab(std::size_t index) {
  operations_.activate_tab(index);
}

bool WorkspaceActionContext::HasOpenTabs() const {
  return !state_.focused_group().open_tabs.empty();
}

std::size_t WorkspaceActionContext::OpenTabCount() const {
  return state_.focused_group().open_tabs.size();
}

std::size_t WorkspaceActionContext::ActiveTabIndex() const {
  return state_.focused_group().active_tab_index;
}

void WorkspaceActionContext::MoveActiveTabTo(std::size_t index) {
  operations_.move_active_tab_to(index);
}

void WorkspaceActionContext::ReopenActiveTab() {
  operations_.reopen_active_tab();
}

bool WorkspaceActionContext::SaveTab(std::size_t index) {
  return operations_.save_tab(index);
}

bool WorkspaceActionContext::SaveTabAs(std::size_t index, const std::filesystem::path& path,
                                       std::string* error) {
  if (!operations_.save_tab_as) {
    if (error != nullptr) {
      *error = "Save As is unavailable";
    }
    return false;
  }
  return operations_.save_tab_as(index, path, error);
}

bool WorkspaceActionContext::OpenNewBufferInNewTab(const std::filesystem::path& path) {
  return operations_.open_new_buffer_in_new_tab && operations_.open_new_buffer_in_new_tab(path);
}

void WorkspaceActionContext::OpenSaveAsPrompt() {
  if (operations_.open_prompt_surface) {
    operations_.open_prompt_surface(PromptSurfaceState::Action::SaveAs,
                                    PromptSurfaceState::Kind::TextInput, ProjectRoot(),
                                    std::string{});
  }
}

void WorkspaceActionContext::ResetCaretBlink() {
  operations_.reset_caret_blink();
}

bool WorkspaceActionContext::SplitEditorGroup(EditorSplitOrientation orientation,
                                              const std::filesystem::path& path,
                                              std::string* error_message) {
  const bool open_path = !path.empty();
  // A split always carves a NEW group holding a clone of the source tab, so
  // replacing that clone's view is safe: it shares the source buffer and no
  // distinct edits are lost. Open the file first so a failure aborts before
  // mutating the group layout.
  editor::TextViewport opened_view;
  if (open_path && !opened_view.OpenFile(path)) {
    if (error_message != nullptr) {
      *error_message = "Failed to open file: " + path.string();
    }
    return false;
  }
  if (!operations_.split_editor_group(orientation)) {
    if (error_message != nullptr) {
      *error_message = "Failed to split the active editor";
    }
    return false;
  }
  if (open_path && !operations_.replace_active_editor_view(opened_view)) {
    if (error_message != nullptr) {
      *error_message = "Failed to open in the split: " + path.string();
    }
    return false;
  }
  return true;
}

bool WorkspaceActionContext::FocusOtherGroup() {
  return operations_.focus_other_group();
}

bool WorkspaceActionContext::FocusEditorGroupInDirection(EditorGroupDirection direction) {
  return operations_.focus_editor_group_in_direction(direction);
}

bool WorkspaceActionContext::MoveEditorGroupInDirection(EditorGroupDirection direction) {
  return operations_.move_editor_group_in_direction(direction);
}

bool WorkspaceActionContext::CloseEditorGroup() {
  return operations_.close_editor_group();
}

void WorkspaceActionContext::RequestCloseTab(std::size_t index) {
  operations_.request_close_tab(index);
}

void WorkspaceActionContext::RequestCloseTabs(std::vector<std::size_t> indices) {
  operations_.request_close_tabs(std::move(indices));
}

void WorkspaceActionContext::CloseAllTabs() {
  operations_.close_all_tabs();
}

bool WorkspaceActionContext::ExecuteLineNavigation(const LineNavigationRequest& request,
                                                   bool relative) {
  editor::TextViewport* viewport = operations_.active_navigable_viewport();
  if (viewport == nullptr) {
    return false;
  }

  const std::size_t line_count = std::max<std::size_t>(1, viewport->line_count());
  std::size_t line = 0;
  if (relative) {
    const long long current_line = static_cast<long long>(viewport->cursor_line()) + 1;
    const long long target_line = current_line + request.requested_line;
    line = static_cast<std::size_t>(
        std::clamp(target_line - 1, 0LL, static_cast<long long>(line_count - 1)));
  } else if (request.requested_line > 0) {
    line = static_cast<std::size_t>(request.requested_line - 1);
  } else {
    const std::size_t from_end = static_cast<std::size_t>(-request.requested_line);
    line = from_end >= line_count ? 0 : line_count - from_end;
  }

  viewport->JumpCursorTo(line, request.column > 0 ? request.column - 1 : 0);
  state_.surface.focus = FocusTarget::Editor;
  operations_.request_focused_editor_redraw();
  return true;
}

void WorkspaceActionContext::SelectAll() {
  if (operations_.select_all_at_active_single_line_text_surface()) {
    operations_.reset_caret_blink();
    return;
  }
  if (operations_.has_active_single_line_text_surface()) {
    // Active single-line field (possibly empty): consume the shortcut. Falling
    // through would Select-All the background editor and steal focus to it.
    return;
  }
  if (auto* viewport = operations_.active_navigable_viewport(); viewport != nullptr) {
    viewport->SelectAll();
    operations_.reset_caret_blink();
    operations_.request_focused_editor_redraw();
    state_.surface.focus = FocusTarget::Editor;
    return;
  }
}

// Undo and Redo differ in exactly two things: which viewport call they make, and
// the perf-trace name. Everything else — the merge selection capture, the
// fold-model rescan, the compare/merge bookkeeping and the five redraw requests —
// has to stay identical between them, and used to be two 55-line copies, so a fix
// to one silently left the other behind.
void WorkspaceActionContext::ApplyUndoRedo(bool redo) {
  util::PerformanceTrace::Scope perf_scope(redo ? "WorkspaceActionContext::Redo"
                                                : "WorkspaceActionContext::Undo");
  auto* viewport = operations_.active_editable_viewport();
  if (viewport == nullptr) {
    return;
  }

  const bool was_dirty = viewport->dirty();
  const std::size_t cursor_before_line = viewport->cursor_line();
  std::optional<editor::SelectionRange> selection_before;
  std::optional<editor::TextPosition> cursor_before;
  auto* merge_tab = operations_.active_merge_tab();
  const bool viewport_is_merge_result =
      merge_tab != nullptr && viewport == &merge_tab->result_viewport;
  if (viewport_is_merge_result) {
    selection_before = viewport->selection_range();
    cursor_before = editor::TextPosition{viewport->cursor_line(), viewport->cursor_column()};
  }

  bool changed = false;
  {
    util::PerformanceTrace::Scope scope(redo ? "WorkspaceActionContext::Redo::ViewportRedo"
                                             : "WorkspaceActionContext::Undo::ViewportUndo");
    changed = redo ? viewport->Redo() : viewport->Undo();
  }
  // An undo that rewinds past a snippet insertion leaves the session's mirror
  // positions pointing at text that is gone. Redo cannot reach an active session
  // (the undo that made redo possible already dropped it), and clearing an
  // inactive one is a no-op, so this runs unconditionally rather than only on undo.
  if (changed && operations_.clear_active_snippet_session_after_undo) {
    operations_.clear_active_snippet_session_after_undo();
  }
  if (!changed) {
    return;
  }

  // Undo/redo mutate the document like any other edit, so the fold model must be
  // recomputed. The content_revision fingerprint alone does not force a rescan
  // once a file is fully resolved, so mark the fold model dirty explicitly
  // (mirroring NotifyEditorViewportChanged); otherwise restored/removed folds go
  // stale — a phantom fold marker can hide an arbitrary line range.
  if (auto* editor_tab = ActiveEditorTab(); editor_tab != nullptr) {
    editor_tab->folding_model->MarkDirty();
  }
  RefreshActiveCompareAfterViewportEdit();
  if (viewport_is_merge_result) {
    util::PerformanceTrace::Scope scope("WorkspaceActionContext::UndoRedo::UpdateMergeTracking");
    operations_.update_merge_tracking_after_viewport_edit(*merge_tab, selection_before,
                                                          *cursor_before);
  }
  operations_.reset_caret_blink();
  operations_.request_active_tab_redraw(false);
  operations_.request_focused_editor_redraw();
  operations_.request_active_editable_last_change_redraw();
  if (viewport->dirty() != was_dirty) {
    util::PerformanceTrace::Scope scope("WorkspaceActionContext::UndoRedo::DirtyStateSideEffects");
    operations_.request_active_editable_blame_neighborhood_redraw(cursor_before_line,
                                                                  viewport->cursor_line());
    operations_.request_tab_strip_redraw();
  }
}

void WorkspaceActionContext::Undo() { ApplyUndoRedo(/*redo=*/false); }

void WorkspaceActionContext::Redo() { ApplyUndoRedo(/*redo=*/true); }

std::string WorkspaceActionContext::CopySelectionText() const {
  // A focused single-line field owns the keystroke even when the panel holds
  // focus: the terminal find bar floats over a terminal that may itself have a
  // selection, and Ctrl+C there must copy the query, not the transcript.
  if (operations_.has_active_single_line_text_surface()) {
    // A single-line field owns the keystroke. Return its selection (empty when
    // nothing is selected -> the caller skips the clipboard write) instead of
    // falling through to the background editor and copying its current line.
    return operations_.selected_text_at_active_single_line_text_surface();
  }
  if (state_.surface.focus == FocusTarget::Panel && operations_.terminal_has_selection()) {
    return operations_.selected_terminal_text();
  }
  if (const auto* viewport = operations_.active_navigable_viewport(); viewport != nullptr) {
    // VSCode multi-caret copy: each caret's selection joined by newline, so a
    // Ctrl-D multi-select copies every occurrence, not just the primary.
    if (viewport->has_multiple_carets()) {
      if (std::optional<std::string> multi = viewport->MultiCaretSelectedText();
          multi.has_value()) {
        return *multi;
      }
    }
    if (viewport->has_selection()) {
      return viewport->SelectedText();
    }
    return viewport->CurrentLineTextForClipboard();
  }
  return {};
}

std::optional<std::string> WorkspaceActionContext::LastTerminalCommandText() const {
  return operations_.last_terminal_command_text();
}

std::optional<std::string> WorkspaceActionContext::SelectionTextWithContext() {
  return operations_.selection_text_with_context();
}

void WorkspaceActionContext::CutSelection() {
  if (operations_.cut_selection_at_active_single_line_text_surface()) {
    operations_.reset_caret_blink();
    return;
  }
  if (operations_.has_active_single_line_text_surface()) {
    // Active single-line field with nothing selected: consume the shortcut.
    // Falling through would DeleteCurrentLine() on the background editor -- an
    // out-of-nowhere data-loss edit behind the focused input surface.
    return;
  }
  if (auto* viewport = operations_.active_editable_viewport(); viewport != nullptr) {
    const bool was_dirty = viewport->dirty();
    // VSCode multi-caret cut: aggregate every caret's selection for the clipboard
    // and delete them all in one undo step. Falls back to primary selection / line.
    const std::optional<std::string> multi_text =
        viewport->has_multiple_carets() ? viewport->MultiCaretSelectedText() : std::nullopt;
    const bool has_selection = viewport->has_selection();
    const std::string text =
        multi_text.has_value()
            ? *multi_text
            : (has_selection ? viewport->SelectedText() : viewport->CurrentLineTextForClipboard());
    // TD-2026-07-17A-119: never delete a selection we cannot fully capture to the
    // clipboard — truncating the copy and then deleting the whole selection would lose
    // data. Refuse an over-budget cut (leave the buffer intact) and notify.
    if (text.size() > kMaxClipboardExportBytes) {
      if (operations_.notify) {
        operations_.notify(NotificationService::Tone::Warning,
                           "Selection too large to cut");
      }
      return;
    }
    if (!text.empty() && operations_.write_clipboard_text(text)) {
      operations_.write_primary_selection_text(text);
      const std::size_t cursor_before_line = viewport->cursor_line();
      std::optional<editor::SelectionRange> selection_before;
      std::optional<editor::TextPosition> cursor_before;
      if (auto* merge_tab = operations_.active_merge_tab();
          merge_tab != nullptr && viewport == &merge_tab->result_viewport) {
        selection_before = viewport->selection_range();
        cursor_before = editor::TextPosition{viewport->cursor_line(), viewport->cursor_column()};
      }
      if (multi_text.has_value()) {
        viewport->DeleteMultiCaretSelections();
      } else if (has_selection) {
        viewport->DeleteSelectedText();
      } else {
        viewport->DeleteCurrentLine();
      }
      RefreshActiveCompareAfterViewportEdit();
      if (auto* merge_tab = operations_.active_merge_tab();
          merge_tab != nullptr && viewport == &merge_tab->result_viewport) {
        operations_.update_merge_tracking_after_viewport_edit(*merge_tab, selection_before,
                                                              *cursor_before);
      }
      operations_.reset_caret_blink();
      operations_.request_active_tab_redraw(false);
      operations_.request_focused_editor_redraw();
      operations_.request_active_editable_last_change_redraw();
      if (viewport->dirty() != was_dirty) {
        operations_.request_active_editable_blame_neighborhood_redraw(cursor_before_line,
                                                                      viewport->cursor_line());
        operations_.request_tab_strip_redraw();
      }
    }
  }
}

void WorkspaceActionContext::PasteClipboard() {
  if (std::optional<std::string> clipboard_text = operations_.read_clipboard_text();
      clipboard_text.has_value()) {
    InsertTextIntoActiveSurface(std::move(*clipboard_text), /*distribute_across_carets=*/true);
  }
}

void WorkspaceActionContext::InsertText(std::string text) {
  InsertTextIntoActiveSurface(std::move(text));
}

void WorkspaceActionContext::InsertTextIntoActiveSurface(std::string text,
                                                         bool distribute_across_carets) {
  // Cap a pathologically large insertion before it is normalized, line-split
  // (one std::string per line), inserted, and stored in the undo history. A
  // multi-hundred-MB paste or `type` payload would otherwise amplify into a
  // multi-GB transient and undo entry -> OOM from a single gesture. 64 MiB is
  // far beyond any interactive paste or typed argument.
  constexpr std::size_t kMaxInsertBytes = 64u << 20;
  if (text.size() > kMaxInsertBytes) {
    text.resize(util::PreviousUtf8Boundary(text, kMaxInsertBytes));
  }
  if (state_.surface.focus == FocusTarget::Panel && operations_.active_terminal_tab() != nullptr) {
    operations_.paste_text_into_terminal(std::move(text));
    return;
  }
  if (operations_.insert_text_into_active_text_surface(text)) {
    return;
  }
  if (auto* viewport = operations_.active_editable_viewport(); viewport != nullptr) {
    const bool was_dirty = viewport->dirty();
    const std::size_t cursor_before_line = viewport->cursor_line();
    std::optional<editor::SelectionRange> selection_before;
    std::optional<editor::TextPosition> cursor_before;
    if (auto* merge_tab = operations_.active_merge_tab();
        merge_tab != nullptr && viewport == &merge_tab->result_viewport) {
      selection_before = viewport->selection_range();
      cursor_before = editor::TextPosition{viewport->cursor_line(), viewport->cursor_column()};
    }
    if (distribute_across_carets) {
      viewport->PasteText(text);
    } else {
      viewport->InsertText(text);
    }
    RefreshActiveCompareAfterViewportEdit();
    if (auto* merge_tab = operations_.active_merge_tab();
        merge_tab != nullptr && viewport == &merge_tab->result_viewport) {
      operations_.update_merge_tracking_after_viewport_edit(*merge_tab, selection_before,
                                                            *cursor_before);
    }
    operations_.reset_caret_blink();
    operations_.request_active_tab_redraw(false);
    operations_.request_focused_editor_redraw();
    operations_.request_active_editable_last_change_redraw();
    if (viewport->dirty() != was_dirty) {
      operations_.request_active_editable_blame_neighborhood_redraw(cursor_before_line,
                                                                    viewport->cursor_line());
      operations_.request_tab_strip_redraw();
    }
  }
}

void WorkspaceActionContext::RefreshAvailableColorschemeNames() {
  operations_.refresh_available_colorscheme_names();
}

void WorkspaceActionContext::RequestLiveConfigRedraw() {
  if (operations_.note_layout_inputs_changed) {
    operations_.note_layout_inputs_changed();
  }
  if (operations_.request_window_redraw) {
    operations_.request_window_redraw();
  }
}

void WorkspaceActionContext::ToggleWindowFullscreen() {
  if (operations_.request_toggle_fullscreen) {
    operations_.request_toggle_fullscreen();
  }
}

void WorkspaceActionContext::ApplyColorscheme(std::string_view name) {
  operations_.apply_colorscheme(name);
  // A theme change repaints with new colors but does not change geometry; the
  // window redraw is what makes it visible (the shell otherwise idles on events).
  RequestLiveConfigRedraw();
}

std::string_view WorkspaceActionContext::CurrentColorschemeName() const {
  return state_.active_colorscheme_name;
}

void WorkspaceActionContext::SetTabSize(std::size_t value) {
  // Route through the setting chokepoint so the project layer (not just the
  // materialized editor_preferences cache) records the override, keeping the
  // layered "set as default" model consistent with the Settings overlay.
  if (operations_.set_setting_value) {
    operations_.set_setting_value("editor.tab_size", std::to_string(value));
    return;
  }
  // Fallback for contexts without the setting chokepoint: record the override in the
  // project layer too, so SaveConfigState (which persists state.settings, not the
  // editor_preferences cache) does not drop the change on the next launch.
  settings_layer::Upsert(state_.settings, "editor.tab_size", std::to_string(value));
  state_.editor_preferences.tab_size = value;
  operations_.apply_editor_preferences_to_all_tabs();
  operations_.save_config_state();
}

void WorkspaceActionContext::SetIndentWidth(std::size_t value) {
  if (operations_.set_setting_value) {
    operations_.set_setting_value("editor.indent_width", std::to_string(value));
    return;
  }
  settings_layer::Upsert(state_.settings, "editor.indent_width", std::to_string(value));
  state_.editor_preferences.indent_width = value;
  operations_.apply_editor_preferences_to_all_tabs();
  operations_.save_config_state();
}

float WorkspaceActionContext::UiScale() const {
  return ui_scale_;
}

void WorkspaceActionContext::ApplyUiScale(float scale) {
  operations_.apply_ui_scale(scale);
  RequestLiveConfigRedraw();
}

void WorkspaceActionContext::SetSoftTabs(bool enabled) {
  if (operations_.set_setting_value) {
    operations_.set_setting_value("editor.soft_tabs", enabled ? "true" : "false");
    return;
  }
  settings_layer::Upsert(state_.settings, "editor.soft_tabs", enabled ? "true" : "false");
  state_.editor_preferences.soft_tabs = enabled;
  operations_.apply_editor_preferences_to_all_tabs();
  operations_.save_config_state();
}

bool WorkspaceActionContext::SoftWrapEnabled() const {
  return state_.editor_preferences.soft_wrap;
}

void WorkspaceActionContext::SetSoftWrap(bool enabled) {
  if (operations_.set_setting_value) {
    operations_.set_setting_value("editor.wrap", enabled ? "word" : "off");
  } else {
    settings_layer::Upsert(state_.settings, "editor.wrap", enabled ? "word" : "off");
    state_.editor_preferences.soft_wrap = enabled;
    operations_.apply_editor_preferences_to_all_tabs();
    operations_.save_config_state();
  }
  // Wrap toggling reflows every visual row in the active editor; the partial
  // redraw scope from a menu/shortcut close otherwise leaves stale pixels for
  // rows that did not change geometry. Force a full editor-surface redraw.
  operations_.request_active_tab_redraw(false);
}

editor::TextViewport* WorkspaceActionContext::ActiveEditableViewport() {
  return operations_.active_editable_viewport();
}

editor::TextViewport* WorkspaceActionContext::ActiveNavigableViewport() {
  return operations_.active_navigable_viewport();
}

TabEntry::EditorTabState* WorkspaceActionContext::ActiveEditorTab() {
  return operations_.active_editor_tab ? operations_.active_editor_tab() : nullptr;
}

editor::FoldingModel* WorkspaceActionContext::EnsureActiveFoldingModelFresh() {
  return operations_.ensure_active_folding_model_fresh
             ? operations_.ensure_active_folding_model_fresh()
             : nullptr;
}

void WorkspaceActionContext::RefreshActiveCompareAfterViewportEdit() {
  auto* compare_tab = operations_.active_compare_tab();
  if (compare_tab == nullptr || !operations_.refresh_compare_tab_derived_state ||
      !operations_.sync_compare_selection_from_viewport) {
    return;
  }
  util::PerformanceTrace::Scope scope("WorkspaceActionContext::RefreshActiveCompareAfterViewportEdit");
  operations_.refresh_compare_tab_derived_state(*compare_tab);
  operations_.sync_compare_selection_from_viewport(*compare_tab, /*reveal_selection=*/true);
}

void WorkspaceActionContext::NotifyEditorViewportChanged(bool last_change) {
  if (last_change) {
    if (auto* editor_tab = ActiveEditorTab(); editor_tab != nullptr) {
      editor_tab->folding_model->MarkDirty();
    }
    // The active editable viewport is the compare tab's right pane when a compare
    // tab is focused, and the compare surface paints its RIGHT TEXT from the diff
    // model -- not from the viewport. Without this refresh every edit action that
    // is not cut / paste / undo (move-line, copy-line, insert-line, comment
    // toggle, join, sort, format, snippet insert...) mutated the buffer and left
    // the surface showing the pre-edit text until some unrelated event happened to
    // refresh it. Three action sites had grown their own copy of this; they now
    // route here, so a new edit action cannot forget it.
    //
    // The merge result pane's equivalent (UpdateMergeTrackingAfterViewportEdit)
    // needs the selection and caret from BEFORE the edit, which this hook does not
    // have, so it stays at the sites that can capture them.
    RefreshActiveCompareAfterViewportEdit();
  }
  operations_.reset_caret_blink();
  operations_.request_active_tab_redraw(false);
  operations_.request_focused_editor_redraw();
  if (last_change) {
    operations_.request_active_editable_last_change_redraw();
  }
}

void WorkspaceActionContext::NotifyEditorCaretMoved() {
  if (operations_.notify_snippet_session_caret_moved) {
    operations_.notify_snippet_session_caret_moved();
  }
  operations_.reset_caret_blink();
  operations_.request_active_tab_redraw(false);
  operations_.request_focused_editor_redraw();
}

std::optional<std::string> WorkspaceActionContext::GetSettingValue(std::string_view id) const {
  if (!operations_.get_setting_value) {
    return std::nullopt;
  }
  return operations_.get_setting_value(id);
}

bool WorkspaceActionContext::SetSettingValue(std::string_view id, std::string value) {
  if (!operations_.set_setting_value) {
    return false;
  }
  return operations_.set_setting_value(id, std::move(value));
}

void WorkspaceActionContext::Notify(NotificationService::Tone tone, std::string message) {
  if (operations_.notify) {
    operations_.notify(tone, std::move(message));
  }
}

void WorkspaceActionContext::ToggleEditorEssentialsCapability(ActionId id) {
  const char* key = EditorEssentialsCapabilitySettingKey(id);
  if (key == nullptr) {
    return;
  }
  if (!operations_.set_setting_value || !operations_.get_setting_value) {
    return;
  }

  const bool currently_enabled =
      SettingFlagEnabled(operations_.get_setting_value(key), /*default_value=*/true);
  const bool next_enabled = !currently_enabled;
  if (!operations_.set_setting_value(key, next_enabled ? "true" : "false")) {
    return;
  }
  operations_.request_active_tab_redraw(false);
}

bool WorkspaceActionContext::Focus(FocusRequestTarget target) {
  switch (target) {
    case FocusRequestTarget::Sidebar:
      if (state_.sidebar.visible) {
        state_.surface.focus = FocusTarget::Sidebar;
        return true;
      }
      return false;
    case FocusRequestTarget::Editor:
      state_.surface.focus = FocusTarget::Editor;
      return true;
    case FocusRequestTarget::Panel:
      if (operations_.active_terminal_tab() != nullptr) {
        state_.surface.focus = FocusTarget::Panel;
        return true;
      }
      return false;
    case FocusRequestTarget::Unknown:
      return false;
  }
  return false;
}

}  // namespace microide::workspace
