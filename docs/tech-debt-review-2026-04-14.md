# Technical Debt & Code Duplication Review - 2026-04-14

## Overview
This audit identified significant architectural fragmentation and code duplication issues, primarily stemming from the "God Object" pattern applied to the `WorkspaceShell` system and redundant utility implementations across independent subsystems.

---

## 1. Architectural Debt: The `WorkspaceShell` Monolith
### Findings
- The `WorkspaceShell` system is fragmented into over 20 source files (`WorkspaceShell*.cpp`). This fragmentation, while attempting to modularize a central "God Object," has resulted in:
    - **Hidden Coupling:** Multiple `Coordinator` classes and shell fragments share access to private `WorkspaceShell` state, creating brittle dependencies.
    - **Duplicated Constants:** Constants like `kSidebarToolNames`, `kFocusTargetNames`, and `kToggleValues` are duplicated across multiple `WorkspaceShell*.cpp` files.
    - **Leaky Abstractions:** `WorkspaceShellShared` acts as a catch-all header/source pair, violating separation of concerns and increasing recompilation times.

### Impact
- Significant cognitive load for developers navigating the codebase.
- High risk of regression when modifying core shell behavior.
- "Fragile Base Class" syndrome where changes to shell state break multiple disconnected coordinators.

### Recommendations
- **Consolidate State:** Define clear, immutable state interfaces for coordinators instead of passing full `WorkspaceShell` references.
- **Extract Constants:** Move all shared layout and configuration constants into a dedicated `WorkspaceConstants.h`.
- **Refactor Coordinators:** Re-architect coordinators as independent, decoupled services that communicate via events or a central message bus, rather than direct manipulation of shell state.

---

## 2. Code Duplication: Utility Functions
### Findings
- **UTF-8 Handling:** `Utf8SequenceLength` is duplicated in `src/compare/CompareModel.cpp` and `src/workspace/WorkspaceShellShared.cpp`.
- **Line Splitting:** Multiple, slightly different implementations of line-splitting logic exist across `src/editor/TextViewport.cpp` and `src/compare/CompareModel.cpp`.
- **PCRE2 Wrappers:** `ProjectSearchService.cpp` and `RuntimeSyntaxRegistry.cpp` both implement near-identical RAII wrappers for PCRE2 (`CompiledSearchPattern` vs. `CompiledRegex`).
- **Path Manipulation:** Similar path normalization and display logic exists in `src/project/DirectoryTree.cpp` and `src/workspace/WorkspaceShellShared.cpp`.

### Impact
- Inconsistent behavior across the IDE (e.g., different definitions of "lines" or "valid UTF-8").
- Maintenance burden: bug fixes in one location are missed in others.

### Recommendations
- **Centralize Utilities:** Create a `src/util/` library for common string, path, and regex operations. 
- **Standardize Primitives:** Adopt a single, project-wide implementation for core primitives (UTF-8, Line Splitting) and deprecate redundant variants.
- **Refactor Regex:** Abstract the PCRE2 RAII wrapper into a reusable utility class.

---

## 3. Configuration & Persistence
### Findings
- Manual, line-based serialization of configuration structs is implemented in `WorkspaceShellShared.cpp`.
- Parsing logic relies on a shared `ParseCommandLine` function, which creates coupling between UI command parsing and session persistence logic.

### Impact
- Rigid serialization format that is difficult to extend or validate robustly.
- Potential for desynchronization between config schemas and their parsing logic.

### Recommendations
- **Adopt Structured Data:** Migrate session persistence and configuration to a structured, human-readable format like JSON or YAML, using a single, robust parser.
- **Separate Concerns:** Decouple the UI command parser from the configuration serialization logic.

---

## Summary of Immediate Actions
1. [ ] Create `src/util/StringUtil.h` and migrate duplicated string/UTF-8 utilities.
2. [ ] Create `src/workspace/WorkspaceConstants.h` and consolidate duplicated `constexpr` arrays.
3. [ ] Extract a common PCRE2 wrapper into `src/util/RegexUtil.h`.
4. [ ] Standardize line-splitting logic using `TextViewport` as the source of truth, updating `CompareModel` accordingly.
