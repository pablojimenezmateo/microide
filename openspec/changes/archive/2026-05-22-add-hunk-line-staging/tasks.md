## 1. Service And Types

- [x] 1.1 Define patch operation request, target, selection, preview, and result types.
- [x] 1.2 Add `PatchApplyService` behind a narrow workspace/project service interface.
- [x] 1.3 Include repository snapshot and diff model generation IDs in each operation request.

## 2. Patch Construction

- [x] 2.1 Implement hunk patch generation for stage and unstage.
- [x] 2.2 Implement selected-line patch generation with required context handling.
- [x] 2.3 Add unit tests for partial hunk ranges, adjacent selected ranges, CRLF files, UTF-8 text, and rename-with-edit cases.

## 3. Operation Execution

- [x] 3.1 Dispatch Git apply/index operations through the background executor.
- [x] 3.2 Implement stale-generation checks before application and stale-result checks on completion.
- [x] 3.3 Refresh repository state and active diff tabs after operation completion.
- [x] 3.4 Surface structured errors for stale diff, patch did not apply, cancelled, unsupported target, and unknown failure.

## 4. UI Integration And Safety

- [x] 4.1 Add compare/sidebar actions for stage hunk, stage selected lines, unstage hunk, and unstage selected lines.
- [x] 4.2 Add discard hunk and discard selected-lines preview prompts with explicit confirmation.
- [x] 4.3 Disable patch actions for binary, submodule, and unsupported diff targets.

## 5. Verification

- [x] 5.1 Add integration tests for stage hunk, stage selected lines, unstage hunk, discard hunk, and stale patch failure.
- [x] 5.2 Add a perf scenario or extend the planned perf-gates change for `diff_stage_hunk_large_patch` and `diff_stage_selected_lines`.
- [x] 5.3 Run focused Git, compare, patch service, and architectural-lint tests.
