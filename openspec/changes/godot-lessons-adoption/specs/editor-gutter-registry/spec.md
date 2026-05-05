## ADDED Requirements

### Requirement: Host-Owned Gutter Registry With Typed Columns

The editor host SHALL expose a single `GutterRegistry` service that owns the registration, layout, hit testing, and paint order of every gutter column. Host subsystems and Lua plugins SHALL contribute columns through the registry only; no contributor SHALL draw a gutter directly on the editor render path.

#### Scenario: Registry is the only contribution seam
- **WHEN** a host subsystem or plugin needs to display a per-line gutter element (text, icon, or custom-drawn glyph)
- **THEN** it SHALL call `GutterRegistry::AddColumn(GutterDescriptor)` with a stable id, a typed kind (`TEXT`, `ICON`, or `CUSTOM_DRAW`), a width policy, and the data callback appropriate to the kind
- **AND** the editor render path SHALL iterate the registry's ordered column list rather than calling into individual contributors

#### Scenario: Removing a contributor removes its column
- **WHEN** a contributor calls `GutterRegistry::RemoveColumn(id)` with an id it previously registered
- **THEN** the column SHALL no longer appear in the registry's iteration order on the next paint
- **AND** removing an unknown id SHALL be a no-op (returning `false`) and SHALL NOT log an error

#### Scenario: Registry constructor takes service interfaces, not the shell
- **WHEN** `GutterRegistry` is constructed
- **THEN** its constructor SHALL accept only the narrow service interfaces it needs (e.g., a measurement-cache reader and a notification emitter)
- **AND** it SHALL NOT take a `WorkspaceShell&` or `WorkspaceShell*` parameter

### Requirement: Typed Gutter Column Kinds

The `GutterRegistry` SHALL support three column kinds: `TEXT`, `ICON`, and `CUSTOM_DRAW`. Each kind SHALL have a typed contributor interface that produces only the data the host needs to paint that kind, never raw shell internals.

#### Scenario: TEXT column produces a string per line
- **WHEN** a `TEXT` column is registered with a `std::function<std::string_view(LineIndex)>` provider
- **THEN** the editor render path SHALL invoke the provider once per visible line
- **AND** SHALL paint the returned string clipped to the column width using the host text renderer

#### Scenario: ICON column produces an icon handle per line
- **WHEN** an `ICON` column is registered with a `std::function<IconHandle(LineIndex)>` provider
- **THEN** the editor render path SHALL invoke the provider once per visible line
- **AND** SHALL paint the icon centered in the column using the host icon renderer
- **AND** an empty `IconHandle` SHALL render as no icon for that line

#### Scenario: CUSTOM_DRAW column receives a clipped draw context
- **WHEN** a `CUSTOM_DRAW` column is registered with a `std::function<void(GutterDrawContext&, LineIndex)>` callback
- **THEN** the editor render path SHALL invoke the callback once per visible line with a `GutterDrawContext` clipped to the column's pixel bounds for that line
- **AND** the callback SHALL NOT receive or be able to obtain the raw editor surface or the workspace shell

### Requirement: Lua Plugin Gutter API Routes Through The Registry

The Lua plugin host SHALL expose a `gutter.add(...)` style API that registers a column on the same `GutterRegistry` used by host subsystems. The Lua API SHALL NOT expose any path for plugins to draw outside the registry.

#### Scenario: Plugin adds a TEXT column
- **WHEN** a Lua plugin calls `microide.gutter.add{ id = "lint.severity", kind = "text", width = 16, provider = function(line) return ... end }`
- **THEN** the plugin host SHALL register a `TEXT` column with that id and the provider closure
- **AND** the column SHALL appear in the editor on the next paint without further host changes

#### Scenario: Plugin removes its column on unload
- **WHEN** a Lua plugin is unloaded
- **THEN** every column id registered by that plugin SHALL be removed from the registry
- **AND** subsequent paints SHALL NOT invoke any of the unloaded plugin's providers

### Requirement: Per-Line Measurement Cache With Explicit Dirty Flag

Each line in the editor text buffer SHALL own its measured-text cache (pixel width and, where applicable, glyph runs and wrap break offsets). Each line SHALL expose a `measure_dirty` flag that mutators flip on edit. A host helper SHALL recompute the cache on demand and clear the flag.

#### Scenario: Edit flips the dirty flag for affected lines only
- **WHEN** an edit modifies the content of one or more lines
- **THEN** the `measure_dirty` flag SHALL be set to `true` on those lines and only those lines
- **AND** the flag on unmodified lines SHALL remain unchanged

#### Scenario: Measurement is performed at most once per dirty cycle
- **WHEN** `EnsureMeasured(LineIndex)` is called on a line whose `measure_dirty` is `true`
- **THEN** the measurement helper SHALL run the SDL3_ttf measurement once
- **AND** SHALL store the result in the line's cache
- **AND** SHALL clear the `measure_dirty` flag
- **AND** subsequent `EnsureMeasured` calls on the same line SHALL be no-ops until the flag is set again

#### Scenario: Cache is read-only to consumers
- **WHEN** a consumer (the editor render path, the gutter registry's layout pass, or any other reader) needs a line's pixel width or glyph runs
- **THEN** it SHALL read the values directly from the cache via a `const`-qualified accessor
- **AND** it SHALL NOT trigger measurement; if the cache is dirty, the consumer SHALL call `EnsureMeasured` first as an explicit, named step

### Requirement: Render Path Forbids Measurement During Paint

The editor render translation units SHALL NOT call any measurement entry point during paint. The architectural-lint test SHALL enforce this rule by rejecting render TUs that reference the measurement function names.

#### Scenario: Lint rejects a render TU that calls a measurement function
- **WHEN** the architectural-lint test inspects a render TU and finds a reference to a forbidden measurement entry point (e.g., `MeasureLine`, `ShapeLine`, or any equivalent host text measurement API)
- **THEN** the lint SHALL fail with a message identifying the file and the forbidden symbol
- **AND** the test SHALL pass when the render TU only reads from the cached values

#### Scenario: Lint has a positive and negative fixture
- **WHEN** the lint test itself runs
- **THEN** it SHALL include a fixture file that contains a forbidden call (asserting the lint flags it) and a fixture file that contains only cache reads (asserting the lint accepts it)

### Requirement: Phase A Migration Validates The Registry End-To-End

The blame gutter SHALL be migrated onto `GutterRegistry` in this change as the canary contributor; the other in-tree gutter contributors (diagnostics, breakpoints, fold marks) MAY remain on their existing wiring until a follow-up change.

#### Scenario: Blame gutter is registered through the registry
- **WHEN** a project with git blame data is opened
- **THEN** the blame column SHALL appear via a `TEXT` (or `CUSTOM_DRAW` if needed for color) registration on `GutterRegistry`
- **AND** the editor render path SHALL NOT contain any blame-specific draw code

#### Scenario: Existing gutter contributors keep working unchanged
- **WHEN** a project is opened that exercises diagnostics, breakpoints, or fold marks
- **THEN** those gutters SHALL continue to display through their existing wiring during Phase A
- **AND** their migration SHALL be tracked as a follow-up change
