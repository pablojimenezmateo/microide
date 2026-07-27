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

// Negative + positive control for CheckRegisteredSettingsAreRead: a setting the
// overlay shows and persists while nothing reads it.
void RunRegisteredSettingsAreReadRuleFixtures();

// Control for the shared missing-target guard every file-scanning rule now goes
// through. A rule pointed at a file that no longer exists scans nothing and
// reports green; this pins that such a rule reports its target as missing
// instead, and reports nothing when the target is there.
void RunMissingRuleTargetFixtures();

}  // namespace microide::tests::architecture
