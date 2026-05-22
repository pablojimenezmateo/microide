## 1. Compare Mode Model

- [x] 1.1 Add explicit compare review mode metadata to compare tab state and persistence where required.
- [x] 1.2 Add semantic diff file metadata for rename, mode, binary, line-ending, and submodule changes.
- [x] 1.3 Add tests for working-tree, commit, branch, and conflict-review mode construction.

## 2. Presentation Layer

- [x] 2.1 Introduce presentation state for collapsed regions, inline highlights, metadata rows, and sticky hunk headers.
- [x] 2.2 Implement context expand/collapse while preserving selection and line mapping.
- [x] 2.3 Implement inline word diff caching keyed by diff model generation.
- [x] 2.4 Add whitespace visualization and ignore-whitespace controls with correct invalidation behavior.

## 3. Review Actions

- [x] 3.1 Add next/previous changed-file commands for compare review.
- [x] 3.2 Tighten next/previous hunk behavior across collapsed regions and metadata rows.
- [x] 3.3 Add open-corresponding-file, copy-path, copy-hunk-patch, and copy-file-patch actions.

## 4. Verification

- [x] 4.1 Add regression fixtures for rename-with-edit, binary file, mode change, line-ending-only change, submodule pointer change, and huge generated file.
- [x] 4.2 Add compare interaction tests for context expansion, inline highlight stability, and open-at-line mapping.
- [x] 4.3 Run compare model, compare render, and targeted perf scenarios for compare open/scroll/navigation.
