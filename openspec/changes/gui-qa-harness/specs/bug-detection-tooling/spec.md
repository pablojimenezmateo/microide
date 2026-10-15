## ADDED Requirements

### Requirement: GUI QA Lane

`tools/run-checks.sh` SHALL provide a `qa` lane that builds the application, runs the local GUI QA scenario suite and a fixed-seed monkey run headlessly with the redraw verifier on, writes its log to `/tmp/microide-qa.log`, and fails on any finding. When Xvfb, xdotool or xwd is missing the lane SHALL fail with a message naming the missing tool rather than pass. The remote suite SHALL run only when `MQA_REMOTE_HOST` is set.

#### Scenario: The lane fails on a finding
- **WHEN** `tools/run-checks.sh qa` runs and any scenario or the monkey records a finding
- **THEN** the lane exits non-zero and the log names the finding, the action before it, and the screenshot path

#### Scenario: A missing tool is not a pass
- **WHEN** the lane runs on a machine without Xvfb
- **THEN** it exits non-zero naming Xvfb
