#pragma once

// Self-tests for the architecture rules themselves: synthetic source trees that
// pin what each rule MUST flag and what it MUST accept. A rule whose pattern
// silently stops matching its own call form otherwise keeps passing forever —
// the close-on-exec rule shipped exactly that way (see
// RunDescriptorCloseOnExecRuleFixtures).
//
// These live here rather than in tests/ArchitectureInvariantsTests.cpp because
// that file is capped as a dispatcher by CheckArchitectureInvariantsDispatcherSize.

namespace microide::tests::architecture {

// Negative + positive control for CheckDescriptorCreationIsCloseOnExec.
void RunDescriptorCloseOnExecRuleFixtures();

// Negative + positive control for CheckTerminalSessionNoExtractedImpl, whose
// line-anchored patterns need std::regex::multiline to match at all.
void RunTerminalExtractedImplRuleFixtures();

// Negative + positive control for CheckNoDirectGitRepositoryInWorkspace, whose
// pattern originally matched only a temporary construction.
void RunDirectGitRepositoryRuleFixtures();

// Negative + positive control for CheckEveryActionIdIsReachable: an action named
// only in `case` labels cannot be invoked by anyone.
void RunActionIdReachabilityRuleFixtures();

}  // namespace microide::tests::architecture
