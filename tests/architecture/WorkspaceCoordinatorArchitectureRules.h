#pragma once

#include "architecture/ArchitectureRuleHelpers.h"

#include <cstddef>
#include <filesystem>
#include <vector>

namespace microide::tests::architecture {

RuleResult CheckNoSynchronousSubprocessWaitInWorkspace(const std::filesystem::path& repo_root);
RuleResult CheckLspDidOpenIsNonBlocking(const std::filesystem::path& repo_root);
RuleResult CheckTextViewportNoCombinedLayoutRevision(const std::filesystem::path& repo_root);
RuleResult CheckNoLegacyPersistenceSymbols(const std::filesystem::path& repo_root);
RuleResult CheckNoExecutorPostThenFutureGetInWorkspace(const std::filesystem::path& repo_root);
RuleResult CheckNoSynchronousSubprocessInWorkspace(const std::filesystem::path& repo_root);
RuleResult CheckNoDirectGitRepositoryInWorkspace(const std::filesystem::path& repo_root);
RuleResult CheckOverlayDismissalIsCentralized(const std::filesystem::path& repo_root);
RuleResult CheckEditorGroupsMutateWithTheSplitTree(const std::filesystem::path& repo_root);
RuleResult CheckReactivationDoesNotReloadPlugins(const std::filesystem::path& repo_root);
RuleResult CheckNoFallbackEditorViewportSymbols(const std::filesystem::path& repo_root);
RuleResult CheckCoordinatorOperationsAreCalled(const std::filesystem::path& repo_root);
RuleResult CheckRedrawHooksAreRequestFrame(const std::filesystem::path& repo_root);
RuleResult CheckFactoryHooksDoNotForwardToServices(const std::filesystem::path& repo_root);
RuleResult CheckFactoryHooksDoNotForwardToServicesAtCap(const std::filesystem::path& repo_root,
                                                        std::size_t cap);
std::size_t CountServiceForwardingHooks(const std::filesystem::path& repo_root,
                                        std::vector<Violation>* where);

inline constexpr std::size_t kMaxServiceForwardingHooks = 43;

RuleResult CheckWorkspaceGitCallsRatchetDown(const std::filesystem::path& repo_root);
RuleResult CheckWorkspaceGitCallsRatchetDownAtCap(const std::filesystem::path& repo_root, std::size_t cap);
std::size_t CountShellThreadGitCalls(const std::filesystem::path& repo_root, std::vector<Violation>* where);

inline constexpr std::size_t kMaxShellThreadGitCalls = 26;

void RunExecutorPostThenFutureGetRuleFixtures();
void RunServiceDirectionRuleFixtures();

}
