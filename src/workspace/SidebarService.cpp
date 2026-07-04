#include "workspace/SidebarService.h"

#include <utility>

namespace microide::workspace {

SidebarService::SidebarService(SidebarCoordinator coordinator)
    : coordinator_(std::move(coordinator)) {}

void SidebarService::ShowMode(SidebarMode mode, bool temporary) {
  coordinator_.ShowMode(mode, temporary);
}

void SidebarService::ShowTree(const std::filesystem::path& root) {
  coordinator_.ShowTree(root);
}

void SidebarService::ShowSearch(std::string query, bool temporary) {
  coordinator_.ShowSearch(std::move(query), temporary);
}

void SidebarService::ShowProblems() {
  coordinator_.ShowProblems();
}

void SidebarService::ShowGit() {
  coordinator_.ShowGit();
}

void SidebarService::ShowTests() {
  coordinator_.ShowTests();
}

void SidebarService::ShowOutline() {
  coordinator_.ShowOutline();
}

bool SidebarService::ShowPlugin(std::string_view id, bool temporary) {
  return coordinator_.ShowPlugin(id, temporary);
}

void SidebarService::Close() {
  coordinator_.Close();
}

void SidebarService::Toggle() {
  coordinator_.Toggle();
}

void SidebarService::RestorePrevious() {
  coordinator_.RestorePrevious();
}

void SidebarService::RefreshProjectFiles() {
  coordinator_.RefreshProjectFiles();
}

void SidebarService::RefreshGit() {
  coordinator_.RefreshGit();
}

bool SidebarService::RefreshProblems() {
  return coordinator_.RefreshProblems();
}

bool SidebarService::RefreshTests() {
  return coordinator_.RefreshTests();
}

bool SidebarService::RefreshPlugin() {
  return coordinator_.RefreshPlugin();
}

void SidebarService::RevealSelectedTreeLine() {
  coordinator_.RevealSelectedTreeLine();
}

void SidebarService::RevealSelectedGitLine() {
  coordinator_.RevealSelectedGitLine();
}

void SidebarService::RevealSelectedProblemsLine() {
  coordinator_.RevealSelectedProblemsLine();
}

void SidebarService::RevealSelectedTestsLine() {
  coordinator_.RevealSelectedTestsLine();
}

void SidebarService::RevealSelectedPluginLine() {
  coordinator_.RevealSelectedPluginLine();
}

void SidebarService::MoveGitSelection(int delta) {
  coordinator_.MoveGitSelection(delta);
}

void SidebarService::MoveProblemsSelection(int delta) {
  coordinator_.MoveProblemsSelection(delta);
}

void SidebarService::MoveTestsSelection(int delta) {
  coordinator_.MoveTestsSelection(delta);
}

void SidebarService::MovePluginSelection(int delta) {
  coordinator_.MovePluginSelection(delta);
}

bool SidebarService::OpenGitEntry(std::size_t entry_index) {
  return coordinator_.OpenGitEntry(entry_index);
}

bool SidebarService::OpenProblemItem() {
  return coordinator_.OpenProblemItem();
}

bool SidebarService::OpenTestItem() {
  return coordinator_.OpenTestItem();
}

bool SidebarService::RunTestItem() {
  return coordinator_.RunTestItem();
}

bool SidebarService::OpenPluginItem() {
  return coordinator_.OpenPluginItem();
}

bool SidebarService::TogglePluginItem() {
  return coordinator_.TogglePluginItem();
}

bool SidebarService::CanStageAllGitEntries() const {
  return coordinator_.CanStageAllGitEntries();
}

bool SidebarService::CanDiscardAllGitEntries() const {
  return coordinator_.CanDiscardAllGitEntries();
}

bool SidebarService::StageAllGitEntries() {
  return coordinator_.StageAllGitEntries();
}

void SidebarService::OpenDiscardAllGitPrompt() {
  coordinator_.OpenDiscardAllGitPrompt();
}

bool SidebarService::DiscardAllGitEntries() {
  return coordinator_.DiscardAllGitEntries();
}

bool SidebarService::StageGitEntry(std::size_t entry_index) {
  return coordinator_.StageGitEntry(entry_index);
}

bool SidebarService::UnstageGitEntry(std::size_t entry_index) {
  return coordinator_.UnstageGitEntry(entry_index);
}

bool SidebarService::DiscardGitEntry(std::size_t entry_index,
                                     const std::optional<std::filesystem::path>& expected_path) {
  return coordinator_.DiscardGitEntry(entry_index, expected_path);
}

void SidebarService::OpenDiscardGitEntryPrompt(std::size_t entry_index) {
  coordinator_.OpenDiscardGitEntryPrompt(entry_index);
}

bool SidebarService::DispatchGitSidebarAction(GitSidebarActionId action, std::size_t entry_index) {
  return coordinator_.DispatchGitSidebarAction(action, entry_index);
}

void SidebarService::ReconcileOpenTabsAfterPathDiscard(const std::filesystem::path& path) {
  coordinator_.ReconcileOpenTabsAfterPathDiscard(path);
}

}  // namespace microide::workspace
