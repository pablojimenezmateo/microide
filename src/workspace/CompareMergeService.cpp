#include "workspace/CompareMergeService.h"

#include <utility>

namespace microide::workspace {

CompareMergeService::CompareMergeService(DiffTabCoordinator diff_tabs,
                                         CompareInteractionCoordinator interactions)
    : diff_tabs_(std::move(diff_tabs)), interactions_(std::move(interactions)) {}

std::optional<std::size_t> CompareMergeService::FindOpenCompareTabIndex(
    const std::filesystem::path& path, std::string_view left_ref, std::string_view right_ref) const {
  return diff_tabs_.FindOpenCompareTabIndex(path, left_ref, right_ref);
}

std::optional<std::size_t> CompareMergeService::FindOpenMergeTabIndex(
    const std::filesystem::path& path) const {
  return diff_tabs_.FindOpenMergeTabIndex(path);
}

void CompareMergeService::OpenComparison(const project::GitCommitEntry& commit) {
  diff_tabs_.OpenComparison(commit);
}

bool CompareMergeService::OpenPlainComparison(CompareInput left, CompareInput right) {
  return diff_tabs_.OpenPlainComparison(std::move(left), std::move(right));
}

bool CompareMergeService::OpenMergeEditor(const std::filesystem::path& base_path,
                                          const std::filesystem::path& incoming_path,
                                          const std::filesystem::path& current_path,
                                          const std::filesystem::path& output_path) {
  return diff_tabs_.OpenMergeEditor(base_path, incoming_path, current_path, output_path);
}

bool CompareMergeService::OpenWorkingTreeComparison(const std::filesystem::path& path,
                                                    const std::string& left_ref,
                                                    const std::string& left_label) {
  return diff_tabs_.OpenWorkingTreeComparison(path, left_ref, left_label);
}

bool CompareMergeService::OpenBranchHeadComparison(const std::filesystem::path& path,
                                                   const std::string& left_ref,
                                                   const std::string& left_label,
                                                   const std::string& right_ref,
                                                   const std::string& right_label) {
  return diff_tabs_.OpenBranchHeadComparison(path, left_ref, left_label, right_ref, right_label);
}

bool CompareMergeService::OpenGitConflictMerge(const std::filesystem::path& path) {
  return diff_tabs_.OpenGitConflictMerge(path);
}

void CompareMergeService::OpenPicker() {
  interactions_.OpenPicker();
}

bool CompareMergeService::OpenPickerForPath(const std::filesystem::path& path,
                                            std::string_view commit_spec) {
  return interactions_.OpenPickerForPath(path, commit_spec);
}

void CompareMergeService::OpenOutgoingBasePicker() {
  interactions_.OpenOutgoingBasePicker();
}

void CompareMergeService::ApplyFileHistoryResult(const project::GitFileHistoryResult& history) {
  interactions_.ApplyFileHistoryResult(history);
}

void CompareMergeService::ApplyOutgoingBaseResult(
    const std::vector<project::GitBranchReference>& branches,
    const std::vector<project::GitCommitEntry>& commits) {
  interactions_.ApplyOutgoingBaseResult(branches, commits);
}

void CompareMergeService::RefreshPicker() {
  interactions_.RefreshPicker();
}

void CompareMergeService::MovePickerSelection(int delta) {
  interactions_.MovePickerSelection(delta);
}

void CompareMergeService::OpenSelectedCommit() {
  interactions_.OpenSelectedCommit();
}

void CompareMergeService::OpenWorkingFileFromCompare() {
  interactions_.OpenWorkingFileFromCompare();
}

void CompareMergeService::OpenMergeResultFile() {
  interactions_.OpenMergeResultFile();
}

void CompareMergeService::MoveCompareSelection(int delta) {
  interactions_.MoveCompareSelection(delta);
}

void CompareMergeService::JumpCompareHunk(int delta) {
  interactions_.JumpCompareHunk(delta);
}

void CompareMergeService::JumpCompareReviewFile(int delta) {
  interactions_.JumpCompareReviewFile(delta);
}

void CompareMergeService::CopyComparePath() {
  interactions_.CopyComparePath();
}

void CompareMergeService::CopyCompareHunkPatch() {
  interactions_.CopyCompareHunkPatch();
}

void CompareMergeService::CopyCompareFilePatch() {
  interactions_.CopyCompareFilePatch();
}

void CompareMergeService::StageCompareHunk() {
  interactions_.StageCompareHunk();
}

void CompareMergeService::StageCompareSelectedLines() {
  interactions_.StageCompareSelectedLines();
}

void CompareMergeService::UnstageCompareHunk() {
  interactions_.UnstageCompareHunk();
}

void CompareMergeService::UnstageCompareSelectedLines() {
  interactions_.UnstageCompareSelectedLines();
}

void CompareMergeService::OpenDiscardCompareHunkPrompt() {
  interactions_.OpenDiscardCompareHunkPrompt();
}

void CompareMergeService::OpenDiscardCompareSelectedLinesPrompt() {
  interactions_.OpenDiscardCompareSelectedLinesPrompt();
}

void CompareMergeService::ToggleCompareIgnoreWhitespace() {
  interactions_.ToggleCompareIgnoreWhitespace();
}

void CompareMergeService::ToggleCompareShowWhitespace() {
  interactions_.ToggleCompareShowWhitespace();
}

void CompareMergeService::ScrollCompareRows(int delta) {
  interactions_.ScrollCompareRows(delta);
}

void CompareMergeService::ScrollCompareColumns(int delta) {
  interactions_.ScrollCompareColumns(delta);
}

void CompareMergeService::MoveMergeSelection(int delta) {
  interactions_.MoveMergeSelection(delta);
}

void CompareMergeService::ScrollMergeColumns(int delta) {
  interactions_.ScrollMergeColumns(delta);
}

void CompareMergeService::ApplyMergeChoice(compare::MergeChoice choice) {
  interactions_.ApplyMergeChoice(choice);
}

void CompareMergeService::ResetMergeHunk() {
  interactions_.ResetMergeHunk();
}

void CompareMergeService::JumpNextUnresolvedMergeConflict() {
  interactions_.JumpNextUnresolvedMergeConflict();
}

void CompareMergeService::ToggleMergeBasePane() {
  interactions_.ToggleMergeBasePane();
}

void CompareMergeService::CopyMergeSideSnippet(bool incoming) {
  interactions_.CopyMergeSideSnippet(incoming);
}

void CompareMergeService::MarkMergeResolved() {
  interactions_.MarkMergeResolved();
}

}  // namespace microide::workspace
