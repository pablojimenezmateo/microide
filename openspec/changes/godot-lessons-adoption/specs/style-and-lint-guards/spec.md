## ADDED Requirements

### Requirement: Header Pragma Lint

The repository SHALL ship a Python lint script `scripts/lint/check_header_pragma.py` that verifies every C/C++ header file uses `#pragma once` for include-guarding and rejects classic `#ifndef`/`#define`/`#endif` guards. CTest and CI SHALL run this lint and SHALL fail the build on violation.

#### Scenario: Header without `#pragma once` fails
- **WHEN** the lint runs against a header file that lacks `#pragma once` as the first non-comment, non-blank directive
- **THEN** the script SHALL exit non-zero
- **AND** SHALL print the offending file path and the missing directive

#### Scenario: Header with classic include guards fails
- **WHEN** the lint runs against a header that uses `#ifndef FOO_H` / `#define FOO_H` / `#endif` style guards
- **THEN** the script SHALL exit non-zero
- **AND** SHALL print the offending file path and a message instructing the author to switch to `#pragma once`

#### Scenario: Compliant header passes
- **WHEN** the lint runs against a header whose first non-comment, non-blank line is `#pragma once`
- **THEN** the script SHALL exit zero for that file

#### Scenario: Lint is exposed as a CTest case
- **WHEN** a developer runs `ctest --test-dir build`
- **THEN** a test case `lint-header-pragma` SHALL exist
- **AND** SHALL invoke `scripts/lint/check_header_pragma.py` over the repository's tracked C/C++ headers

### Requirement: File Format Lint

The repository SHALL ship a Python lint script `scripts/lint/check_file_format.py` that verifies every tracked text file conforms to: no UTF-8 BOM, LF (not CRLF) line endings, no trailing whitespace, exactly one trailing newline. CTest and CI SHALL run this lint and SHALL fail the build on violation.

#### Scenario: File with UTF-8 BOM fails
- **WHEN** the lint encounters a file beginning with the UTF-8 BOM `0xEF 0xBB 0xBF`
- **THEN** the script SHALL exit non-zero
- **AND** SHALL print the offending file path and report `BOM`

#### Scenario: File with CRLF endings fails
- **WHEN** the lint encounters a file containing `\r\n` line endings
- **THEN** the script SHALL exit non-zero
- **AND** SHALL print the offending file path and report `CRLF`

#### Scenario: File with trailing whitespace fails
- **WHEN** the lint encounters a line ending with spaces or tabs before its newline
- **THEN** the script SHALL exit non-zero
- **AND** SHALL print the offending file path and at least one offending line number

#### Scenario: File with missing or extra trailing newline fails
- **WHEN** the lint encounters a file with zero trailing newlines or with two or more trailing newlines
- **THEN** the script SHALL exit non-zero
- **AND** SHALL print the offending file path and report `EOF`

#### Scenario: Compliant file passes
- **WHEN** the lint runs against a file with no BOM, only LF endings, no trailing whitespace, and exactly one trailing newline
- **THEN** the script SHALL exit zero for that file

#### Scenario: Lint is exposed as a CTest case
- **WHEN** a developer runs `ctest --test-dir build`
- **THEN** a test case `lint-file-format` SHALL exist
- **AND** SHALL invoke `scripts/lint/check_file_format.py` over the repository's tracked text files

### Requirement: Include Form Lint

The repository SHALL ship a Python lint script `scripts/lint/check_include_form.py` that verifies every `#include` directive in tracked C/C++ source files: forbids backslashes in the path, requires angle brackets for system/third-party headers, requires double quotes for repository headers, and rejects `..` segments. CTest and CI SHALL run this lint and SHALL fail the build on violation.

#### Scenario: Include with backslash path separator fails
- **WHEN** the lint encounters `#include "foo\bar.h"` or `#include <foo\bar.h>`
- **THEN** the script SHALL exit non-zero
- **AND** SHALL print the offending file path, line number, and offending directive

#### Scenario: Include with `..` segment fails
- **WHEN** the lint encounters an include path containing a `..` path segment
- **THEN** the script SHALL exit non-zero
- **AND** SHALL print the offending file path, line number, and offending directive

#### Scenario: Repository header included with angle brackets fails
- **WHEN** the lint encounters `#include <…>` where the path resolves to a header inside `src/` or `tests/`
- **THEN** the script SHALL exit non-zero
- **AND** SHALL print the offending file path, line number, and a message instructing the author to use double quotes for repository headers

#### Scenario: System header included with double quotes is allowed
- **WHEN** the lint encounters `#include "SDL3/SDL.h"` or any other quote-included path that does not resolve inside the repository
- **THEN** the script MAY warn but SHALL NOT fail; the include is treated as a vendored or system header

#### Scenario: Lint is exposed as a CTest case
- **WHEN** a developer runs `ctest --test-dir build`
- **THEN** a test case `lint-include-form` SHALL exist
- **AND** SHALL invoke `scripts/lint/check_include_form.py` over the repository's tracked C/C++ source files

### Requirement: Curated Clang-Tidy Profile

The repository SHALL ship a `.clang-tidy` configuration file at the repo root that enables an explicit, curated short list of high-signal checks and SHALL scope the analysis to the `src/` and `tests/` trees via a `HeaderFilterRegex`. CI SHALL run clang-tidy against the project (initially in non-blocking mode) on every PR.

#### Scenario: Curated check list is enabled
- **WHEN** clang-tidy is invoked using the repository's `.clang-tidy`
- **THEN** the enabled checks SHALL be exactly the curated list `bugprone-use-after-move`, `bugprone-unchecked-optional-access`, `performance-move-const-arg`, `performance-unnecessary-value-param`, `readability-braces-around-statements`, `readability-redundant-member-init`, `modernize-use-nullptr`, `modernize-use-override` (and SHALL NOT enable any other check via globs)

#### Scenario: HeaderFilterRegex scopes the run
- **WHEN** clang-tidy is invoked using the repository's `.clang-tidy`
- **THEN** the `HeaderFilterRegex` SHALL match only headers under `src/` and `tests/`
- **AND** SHALL NOT analyze headers under `thirdparty/`, `build/`, or any installed system include path

#### Scenario: CI runs clang-tidy non-blocking initially
- **WHEN** a CI build executes against a PR
- **THEN** a clang-tidy job SHALL run against the project using `compile_commands.json`
- **AND** SHALL post any findings as job output
- **AND** SHALL NOT fail the merge until a follow-up change promotes it to blocking

### Requirement: Static Checks Scoped To Changed Files

The static-checks CI workflow SHALL compute the set of files changed in the PR and SHALL pass that set to the file-scoped lints (header-pragma, file-format, include-form, clang-format, clang-tidy). The architectural-invariant test SHALL continue to run against the full repository because it inspects multi-file invariants.

#### Scenario: Lints run only against changed files
- **WHEN** a CI build executes against a PR that modifies five C++ files
- **THEN** the file-scoped lints SHALL be invoked with those five files as input
- **AND** SHALL NOT inspect the rest of the repository's tracked files in that job

#### Scenario: Architectural-invariant test runs against the full repository
- **WHEN** a CI build executes against a PR
- **THEN** the architectural-invariant test SHALL run against the full repository regardless of which files changed
- **AND** SHALL fail the merge on any violation

#### Scenario: Changed-files computation does not introduce supply-chain risk
- **WHEN** the workflow computes the changed file list
- **THEN** it SHALL use either an inline `git diff --name-only <merge-base> HEAD` invocation OR a third-party action pinned to a commit SHA (not a tag)
- **AND** SHALL NOT depend on a third-party action pinned only by mutable tag
