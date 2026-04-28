## Context

The MicroIDE workspace layer accumulated debt during fast feature growth. Concrete state today:

- `src/workspace/WorkspaceShell.h` is 1548 lines and exposes 600+ method declarations.
- `src/workspace/WorkspaceShellTooling.cpp` is 2703 lines, `src/plugin/PluginHost.cpp` is 5074 lines, `src/workspace/WorkspaceActionContext.cpp` is 1086 lines.
- 21+ `Workspace*Coordinator*` types exist; most hold a `WorkspaceShell&` and reach directly into shell internals. `docs/known-tech-debt.md` items 1, 2, and 4 already document this.
- `current_project_state_.text_viewport` still exists alongside `ActiveEditorViewport()` (item 3).
- `WorkspacePersistenceFormat.cpp` (835 lines) parses project state, preferences, and sessions as ad-hoc command lines, with `ParseSizeToken`/`ParseFloatToken`/`ParseIntToken`/`ParseInt64Token` wrapping `std::stoull`/`stoi`/`stof`/`stoll` in `try`/`catch`. ~22 numeric-parse call sites in the tree share that pattern.
- Single-line text editing (insertion, caret, composition) is shared via `WorkspaceTextInputCoordinator`, but selection, backspace, copy/cut, and movement remain duplicated across prompt, command, chat, overlay, and sidebar surfaces (item 7).
- Render-path code in `WorkspaceShellRender*.cpp` queries shell helpers at draw time instead of consuming view models (item 4).

Constraints: priority order is correctness > speed > low CPU > low memory > architectural clarity. Plugin-facing Lua API and on-disk schema migration semantics are user-visible and must be preserved through the cutover even though the underlying file format changes. Performance budgets in `openspec/specs/performance-budgets/spec.md` MUST hold.

Stakeholders: solo maintainer; no external API consumers; plugin authors are dogfood-only at present.

## Goals / Non-Goals

**Goals:**

- Eliminate the workspace god-class structure: `WorkspaceShell` becomes ≤ 400 lines header, ≤ 600 lines impl, with zero `friend class` declarations and no public mutating accessors used by external coordinators.
- Replace shell-back-reference coordinators with narrow injected service interfaces. Coordinators translate input → intent → service call; they never mutate shell state directly.
- Decompose `PluginHost` into a runtime core plus extension-surface modules, each ≤ 800 lines.
- Replace the text-command persistence format with a single typed, schema-versioned, atomically-written, checksum-verified structured format used for project state, preferences, sessions, and conversations.
- Consolidate duplicated single-line edit logic into one `SingleLineEditor` model.
- Consolidate hand-rolled numeric parsing into one non-throwing `util/Parse.*` layer.
- Render functions consume explicit view-model structs, not the shell.
- Bake architectural invariants into CI: lint forbids new `friend class` in workspace code, new `WorkspaceShell&` parameters in coordinators, and `try`/`catch` around `std::sto*`.
- Preserve every performance budget; produce trace evidence for every subsystem cutover.

**Non-Goals:**

- Changing the plugin Lua API surface. Plugin authors should see no behavior change.
- Changing the SDL3 render loop, the retained scene model, or the redraw policy.
- Re-architecting editor text storage, syntax engine, terminal session backend, git services, or LSP/DAP wire protocols. Those are touched only where they cross the workspace seam.
- Adding new product features. This change is internal-quality only.
- Cross-platform host work — already shipped under `host-platform-support`.

## Decisions

### D1. Service-oriented decomposition over "thinner shell with helpers"

The shell is replaced by a small `WorkspaceShell` orchestrator plus typed services owned by the shell and injected into coordinators by interface reference. Services to extract (initial set):

- `EditorTabService` — tab list, active tab, splits, dirty state, save/load, view restore.
- `ProjectCatalogService` — open projects, switch, close, project state activation.
- `PromptSurfaceService` — prompt + dirty-prompt + path-mutation prompt lifecycle.
- `SidebarService` — sidebar mode, refresh, open-or-select.
- `CompareMergeService` — compare/merge tab orchestration and navigation commands.
- `TerminalPanelService` — terminal tabs, focus, panel layout requests.
- `PluginRuntimeService` — already exists; absorbs the remaining plugin lifecycle work currently on the shell.
- `PersistenceService` — load/save the new structured format; no other consumer touches disk for workspace state.
- `RenderViewModelBuilder` — builds per-surface view-model structs from service queries; render code consumes only those structs.

Coordinators accept their needed services through constructor injection (`EditorTabService&`, `PromptSurfaceService&`, …). They never receive `WorkspaceShell&`.

**Alternative considered:** keep `WorkspaceShell` as a façade with narrower public methods. Rejected because the existing "thin façade" attempts (`WorkspaceActionContext`) have grown past 1000 lines and remain shell-coupled; the friend pattern ships back in.

### D2. Persistence: typed structured format, single library, atomic write

The new format is a length-prefixed, tagged, typed record stream:

- File header: magic (`"MIDE"`), 4-byte format version, 4-byte capability flags, 4-byte CRC32C of the body.
- Body: sequence of typed records (`{tag: u16, length: u32, payload}`). Tags namespaced by record type; unknown tags are skipped on read with a warning, enabling forward compatibility.
- Primitive types: `u32`, `i32`, `i64`, `f32`, `bool`, `string` (length-prefixed UTF-8), `path` (string + platform tag), `vec<T>`, `optional<T>`.
- One `PersistedRecordReader`/`Writer` pair, used for `ProjectWorkspaceState`, `UserConfigState`, `WorkspaceSession`, and `ConversationRegistry`. No bespoke per-section parsers.
- Atomic write via `tmp + fsync + rename` on POSIX, equivalent on Windows. Reads fail closed on CRC mismatch and fall back to the previous-version backup file kept under `<state>.bak`.
- Schema-version field is checked first; mismatched majors trigger the one-shot importer, mismatched minors trigger forward-compat skip-on-unknown-tag.

**Alternative considered:** JSON or TOML. Rejected because the existing format is line-by-line text and JSON parsing brings either a dependency or another hand-rolled parser; the structured binary format is smaller, faster to load (matters for startup budget), and forces typed records.

**Alternative considered:** SQLite. Rejected — overkill for the workload, adds dependency, and breaks the atomic-rename model.

### D3. Single-line editor model

A new `editor/SingleLineEditor` value type owns: `std::string buffer`, `std::size_t caret`, `std::optional<Selection> selection`. It exposes `Insert`, `Backspace`, `DeleteForward`, `MoveLeft`, `MoveRight`, `MoveHome`, `MoveEnd`, `SelectAll`, `Cut`, `Copy`, `Paste`, `SetText`. Prompt input, command input, overlay query, and sidebar search route key events through a shared `SingleLineKeyHandler`.

The chat composer remains on `editor::TextViewport` in this change because it is currently multiline (`newline`, vertical movement, page scrolling) and therefore not behaviorally equivalent to the single-line model. Its alignment with shared input primitives is tracked as a separate follow-up.

### D4. Numeric and token parsing

A `util/Parse.*` module exposes non-throwing primitives over `std::from_chars`:

```
std::optional<int>      ParseInt(std::string_view);
std::optional<long long> ParseInt64(std::string_view);
std::optional<float>    ParseFloat(std::string_view);
std::optional<std::size_t> ParseSize(std::string_view);
```

All `try`/`catch (...) { return false; }` numeric-parse blocks are deleted and replaced with `if (auto v = ParseInt(token)) { … }`. A simple grep-based CI check rejects new `try`-with-`std::sto`.

### D5. Render takes view models, not the shell

Each render function (`RenderActiveWorkspaceSurface`, `RenderBottomPanel`, `RenderHoverPopup`, …) takes a typed view-model struct produced by `RenderViewModelBuilder`. Shell helpers are not callable from render code. View models are POD-like and trivially copyable so the render path remains allocation-free. Where today render code reads ten shell fields, the view model contains exactly those ten fields.

### D6. Coordinator pattern, locked down

A coordinator's constructor accepts only:

- the specific service interfaces it needs,
- value-typed input state (`InputContext`, `ActionSource`),
- read-only references to shared resources (theme, text renderer).

A CI lint pass rejects:

- new `friend class` declarations in `src/workspace/*`,
- new constructors taking `WorkspaceShell&`,
- new `try` blocks containing `std::sto`.

Lint is a focused `microide_tests` fixture that walks the source tree with `std::filesystem` + simple regex; no external dependency.

### D7. Migration cutover, single direction

On first launch with the new format, an importer reads any pre-existing text-format files (`project.state`, `user.config`, `session.workspace`, `chat.conversations`) once, writes them in the new format, and renames the originals to `<file>.legacy`. The text-format reader code is deleted in the same change. Two releases later, the `.legacy` files are also removed by a second one-shot cleanup. There is no runtime fallback to the old format.

### D8. Sequencing as independent service extractions, not a big-bang rewrite

The change lands as ~12 sequential extractions (see `tasks.md`), each independently shippable, each preserving full test runs and capturing trace evidence. The architectural lint is added at the end of step 1 so each subsequent step lands within the new invariants. `WorkspaceShell` shrinks monotonically through the sequence; the final step is the deletion of `WorkspaceShellTooling.cpp` and `WorkspaceActionContext.cpp` once their contents are absorbed.

## Risks / Trade-offs

- [Risk] Performance regression from indirection through service interfaces → Mitigation: services are resolved once at construction; hot paths (render, key event dispatch) hold direct references; before/after `MICROIDE_TRACE_REDRAW` and `MICROIDE_STARTUP_TRACE` evidence required per step in `tasks.md`.
- [Risk] Persistence cutover loses data on a crash mid-migration → Mitigation: importer writes new file via `tmp + fsync + rename` first, and only renames the legacy file to `.legacy` after verifying CRC of the new file by re-reading it.
- [Risk] Service decomposition gets the seams wrong and creates new friction → Mitigation: each extraction adds a fixture exercising the seam; if a coordinator needs a method that isn't on the service, that method must be added explicitly to the service contract (no friend escape hatch).
- [Risk] Long sequence of changes destabilizes `main` → Mitigation: each task in `tasks.md` is independently shippable, gated on full `ctest`; merges land one at a time, not a long-lived branch.
- [Trade-off] Binary persistence format is harder to inspect by hand than the old command-style text → accepted because the format is internal and accompanied by an `openspec`-adjacent dump tool (`microide-dump-state` debug subcommand) added alongside the new reader.
- [Trade-off] Plugin host decomposition will momentarily increase total LoC before it decreases (interfaces + impls) → accepted; net LoC after step 7 is targeted ≤ 60% of the current `PluginHost.cpp`.
- [Risk] Forgetting a stale `text_viewport_` caller → Mitigation: the field is deleted, not deprecated; the compiler enforces zero callers.

## Migration Plan

1. **Land new persistence format and importer** behind a feature flag, with shadow writes to the new file format on every save while still reading the old format. Validate round-trip for one release.
2. **Cut over reads** to the new format with the one-shot importer; delete the old reader.
3. **Extract services in dependency order** (`PersistenceService` → `EditorTabService` → `ProjectCatalogService` → `PromptSurfaceService` → `SidebarService` → `CompareMergeService` → `TerminalPanelService` → `PluginRuntimeService` finalization → `RenderViewModelBuilder`).
4. **Rewrite coordinators** against the new service contracts; remove friends.
5. **Decompose `PluginHost`** into runtime core + extension-surface modules.
6. **Land architectural lint** as the final gate.
7. **Two releases later, remove `.legacy` cleanup code.**

Rollback strategy: each task is a separate commit; revert the offending commit. Persistence step 1 (shadow writes) provides a safe rollback window for the format cutover.

## Open Questions

- Should the structured persistence format be little-endian fixed (simpler) or use platform-native endian with a flag (faster on big-endian, but no current target is big-endian)? Current lean: little-endian fixed.
- Should `RenderViewModelBuilder` be invoked once per frame (cached, invalidated on dirty) or rebuilt per surface render (simpler)? Current lean: per-frame rebuild for the first cut, measure, then cache only if profiling shows cost.
- Does `WorkspaceShellTesting.h` (1501 lines) become a thin re-export over the new service interfaces, or is it deleted entirely with tests rewritten against services? Current lean: delete entirely; tests use services directly.
