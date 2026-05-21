# Glossary

Use these terms consistently in plans, docs, code reviews, and commit messages.

## Core Terms

- Workspace Context:
  - The top-level state container for the active shell session. It owns project catalog state, the current project workspace state, interaction state, prompt state, menu state, and related transient UI state.
- Project Catalog:
  - The collection of known project workspaces plus the active project index. Switching projects should preserve each project's owned state instead of rebuilding it ad hoc.
- Project Workspace State:
  - The full per-project state below a project tab, including tabs, tree state, overlays, diagnostics, terminals, search state, and other project-scoped UI or service state.
- Workspace Shell:
  - The app-facing shell facade. It coordinates subsystems, but it is not the blanket extension surface for plugins or new features.

## Editor And Surface Terms

- Editor Tab:
  - A normal file-editing tab backed by the editor model.
- Compare Tab:
  - A tab that presents a side-by-side comparison workflow.
- Merge Tab:
  - A tab that presents conflict resolution or merge-result editing.
- Text Viewport:
  - The byte-oriented editor model that owns document content, viewport behavior, selection, undo or redo state, and syntax-highlight checkpointing.
- Runtime Syntax Snapshot:
  - The in-tree generated syntax data compiled into the host and extended at runtime by host-loaded plugin contributions.

## Host Extension Terms

- Plugin Host:
  - The subsystem that discovers plugins, owns the Lua runtime, dispatches lifecycle events, and exposes narrow plugin-facing capabilities.
- Plugin Runtime:
  - The host-owned path that coordinates plugin loading, asset watching, reload behavior, and contribution bookkeeping.
- Command Registry:
  - The host-owned registry of actionable commands and their metadata.
- Sidebar Provider:
  - A host-rendered sidebar contribution identified by a stable id and label. Built-in views and plugin-contributed views should both route through host-owned registry and presentation paths.
- Output Channel:
  - A host-owned destination for structured plugin or task output that the shell can display consistently.
- Task Provider:
  - A plugin contribution that supplies runnable task definitions such as build, test, or project-local automation.
- Tool Provider:
  - A plugin contribution that declares an external tool the host may download or manage.
- Test Provider:
  - A plugin contribution that discovers tests and reports test run results to the host.

## Rendering And Performance Terms

- Retained Redraw:
  - The redraw model where the shell tracks invalidated regions and coalesces repaint work instead of repainting the entire window for every event.
- Dirty Region:
  - A rectangle or aggregate invalidation area that marks which part of the frame must be repainted.
- Render Primitive:
  - A reusable host-owned drawing building block such as cards, tabs, list rows, text inputs, or chrome decorations.

## Integration Terms

- Project Service:
  - A narrow host-owned service in `src/project/*` that handles filesystem, search, git, blame, compare, or related project integration without depending on shell rendering.
- Platform Service:
  - A narrow host-owned service in `src/platform/*` that wraps OS-facing concerns such as subprocesses, app directories, file watching, and filesystem helpers.
