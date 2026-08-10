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

// Negative + positive control for CheckCoordinatorOperationsAreCalled: a wired
// Operations hook nothing calls, including one whose name is called only on an
// unrelated struct (reads are scoped by include graph, not by name).
void RunCoordinatorOperationsAreCalledRuleFixtures();

// Negative + positive control for the two render-TU text rules. Every
// WorkspaceUiText composer returns a fresh std::string by value, and none of the
// to_string / std::format / `std::string(...) +` patterns names one — which is how
// the project-search sidebar kept composing its empty-state placeholder on every
// repaint. Also pins that literal+identifier concatenation is enforced on the
// surface render TUs, not just the four hot per-row ones.
void RunRenderTuTextCompositionRuleFixtures();

// Control for the shared missing-target guard every file-scanning rule now goes
// through. A rule pointed at a file that no longer exists scans nothing and
// reports green; this pins that such a rule reports its target as missing
// instead, and reports nothing when the target is there.
void RunPaintedScrollbarRuleFixtures();
void RunWheelFocusRuleFixtures();
void RunMissingRuleTargetFixtures();
// Negative + positive control for CheckEveryPerfCounterHasAProducer.
void RunPerfCounterProducerRuleFixtures();
// Negative + positive control for CheckViewportFiletypeGoesThroughTheViewportMemo:
// the two-argument (content-reading) overload must be flagged outside the memo and
// the one-argument (path-only) overload must not be.
void RunViewportFiletypeMemoRuleFixtures();

// Runs every fixture above.
// Negative + positive control for CheckPerfHarnessIsolatesBeforeConstructingTheShell:
// the app-root isolation must precede the Driver declaration, because the Driver
// holds a WorkspaceShell by value and constructing it loads user-level state.
void RunPerfHarnessIsolationOrderRuleFixtures();

// Negative + positive control for CheckPerfMeasureBodiesDoNotBuildTheirOwnInput,
// including the legitimate exemption (construction IS the measured work) and the
// blind case (no Measure body found at all).
void RunPerfMeasureBodyRuleFixtures();

// Negative + positive control for CheckPerfMeasureBodiesDoNotWaitOnWallClock:
// a measured phase whose body blocks on a wall-clock poll loop gates the runner
// rather than the code, plus the legitimate exemption (a wait that provably does
// not allocate while spinning, or an idle that IS the measurement) and the blind
// case (TD-2026-08-10-179).
void RunPerfMeasureWallClockWaitRuleFixtures();

void RunAllRuleFixtures();

}  // namespace microide::tests::architecture
