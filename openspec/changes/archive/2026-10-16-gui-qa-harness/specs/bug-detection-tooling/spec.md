## ADDED Requirements

### Requirement: GUI QA Lane

`tools/run-checks.sh` SHALL provide a `qa` lane that builds the application, runs the local GUI QA scenario suite and a fixed-seed monkey run headlessly with the redraw verifier on, writes its log to `/tmp/microide-qa.log`, and fails on any finding. When Xvfb, xdotool or xwd is missing the lane SHALL fail with a message naming the missing tool rather than pass. The remote suite SHALL run only when `MQA_REMOTE_HOST` is set.

#### Scenario: The lane fails on a finding
- **WHEN** `tools/run-checks.sh qa` runs and any scenario or the monkey records a finding
- **THEN** the lane exits non-zero and the log names the finding, the action before it, and the screenshot path

#### Scenario: A missing tool is not a pass
- **WHEN** the lane runs on a machine without Xvfb
- **THEN** it exits non-zero naming Xvfb

### Requirement: One headless driver

Headless drivers of the shipped binary (random control commands, pointer and keyboard input, repro scenarios) SHALL live in `tools/qa` on one launcher, one isolation recipe and one oracle set. The former `tools/fuzz-control-commands.py` and `tools/repro/overlay-backdrop-repro.sh` SHALL be removed once their function is reachable from `tools/qa/run.py`, and documents that named them SHALL name their replacement.

#### Scenario: The command fuzzer keeps its seeds
- **WHEN** `tools/qa/run.py monkey --profile commands --seed N` runs against the ASAN binary
- **THEN** it drives the same command generators with on-disk churn as the former script did for seed N, and a sanitizer report fails the run
