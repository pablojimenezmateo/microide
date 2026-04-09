# Blame Shadow Text Design

This document defines how per-line git blame shadow text should be added without hurting normal
editor latency.

The short version:

- blame must never run synchronously on typing, scrolling, or repaint
- blame must be scoped to the visible editor window plus a small padded range
- blame must be cached by file and line span
- blame should be disabled for the same kinds of files where syntax highlighting is already cut
  back for performance

## Why This Needs Its Own Design

MicroIDE's editor render path is currently synchronous and hot. `EditorViewRenderer` walks visible
lines and paints text directly on each redraw. That is the wrong place to ask git anything.

The codebase already has one good precedent for background project work: `ProjectSearchService`.
Blame should follow the same separation of concerns, but with stricter request coalescing because
viewport motion is far more frequent than a search query change.

## User-Facing Behavior

Initial scope:

- show muted per-line shadow text only in normal editor tabs
- format each visible line as `Author, relative age • Summary`
- render the text on the right side of the editor, clipped to the available width
- show nothing while data is missing instead of blocking or painting per-line spinners

Initial exclusions:

- no blame in compare tabs, merge tabs, placeholder views, or terminal surfaces
- no blame for untracked files
- no blame while the buffer is dirty
- no blame for files already in large-file mode
- no blame when the pane is too narrow to reserve a readable shadow-text column

The simplest safe first release is to support only clean, tracked, normal editor buffers. That
keeps the output honest and avoids sending full in-memory buffers through git during editing.

## Hard Performance Rules

These are non-negotiable implementation constraints:

- no synchronous `git blame` work on cursor movement, viewport scrolling, or editor redraw
- no subprocess creation from `EditorViewRenderer`
- no render-path mutex waits longer than a short snapshot swap
- no `git blame` request for every scroll tick if the requested lines are already cached
- no blame for files that already trip large-file mode

If any of these are hard to preserve, the feature should stay off until the implementation is
reworked.

## Initial Feature Limits

The initial implementation should be deliberately conservative.

Eligibility rules:

- file is inside the current git repo and is tracked
- buffer has no unsaved edits
- working-tree status for the file is clean
- `TextViewport::large_file_mode()` is false
- file stays under the existing editor large-file thresholds: `384 KiB` or `4000` lines
- pane width can still leave roughly `28` monospace columns for blame text after the code area

These limits mirror the existing syntax-highlighting cutoff strategy on purpose. If syntax gets
disabled for a large buffer, blame should be disabled too.

## Git Command Contract

The default blame collection path should use the system git with a bounded machine-readable format:

```bash
git -C <repo> blame --incremental --encoding=UTF-8 -L <start>,<end> -- <path>
```

Why this shape:

- `--incremental` is explicitly documented for interactive viewers and streams results as they are
  built
- `-L` keeps work bounded to the visible range
- `--encoding=UTF-8` matches the editor's text expectations better than locale-dependent output
- the incremental format omits the source line text, which MicroIDE already has in memory

The default viewport path should not add `-M`, `-C`, or `-w`.

Reasons:

- `-M` and `-C` do extra move or copy detection work and are better treated as future depth knobs,
  not default hot-path behavior
- `-w` can be useful, but it is another dimension of behavior and cost that should not ride along
  with the first release

The parser must tolerate unknown extended tags, matching the official guidance for porcelain-style
parsers.

## Dirty Buffers And `--contents`

Git supports `--contents <file>` and `--contents -`, which means MicroIDE could theoretically blame
the current unsaved buffer contents.

That should not be part of the first implementation.

Reasons:

- it would require serializing the whole buffer for blame requests
- it couples editor edit frequency to blame subprocess work
- it complicates cache keys because every edit can change every downstream line attribution
- it makes it much easier to regress typing latency by accident

For the first version, dirty buffers should simply suppress blame until the file is saved or
reloaded into a clean state.

## Service Architecture

Add a dedicated `src/project/GitBlameService.{h,cpp}`.

Recommended shape:

- one long-lived worker thread
- a condition variable or equivalent wait mechanism
- latest-request wins semantics
- SDL wake events back to the main thread, similar to `ProjectSearchService`

Do not model blame as "spawn a new thread and join the previous one for every request". Scroll and
tab motion happen too often for that to stay cheap.

The main thread should submit a lightweight request describing:

- repo root
- repo-relative file path
- current visible line start
- current visible line count
- document eligibility state
- a cache or document version stamp
- a repository version stamp such as current `HEAD`

The worker should:

- expand the visible range into a padded request window
- skip any lines already covered by cache
- run blame only for missing spans
- publish partial or completed results back to the UI

## Viewport Scope And Prefetch

Blame should be line-addressed, not whole-file addressed.

Recommended initial request policy:

- visible range is the core target
- add padding of `max(visible_lines, 128)` lines around it
- cap a single request window to `512` lines total
- debounce uncached scroll-driven expansions by about `75 ms`

This keeps nearby scrolling warm without turning every file visit into a whole-file blame.

Example:

- visible lines `400-460`
- padded request becomes roughly `272-588`
- if cache already has `272-520`, only `521-588` needs a new blame request

## Cache Model

Cache per eligible file, not per render frame.

Each file cache should store:

- sparse loaded line spans
- per-line blame entries for loaded spans
- deduplicated commit metadata keyed by commit id
- a small preformatted display string for each visible-line entry
- last access time for LRU eviction

Recommended initial cache key:

- repo root
- repo-relative file path
- current `HEAD` id
- file size or mtime stamp from the clean on-disk file

Recommended initial cache budget:

- at most `8` file caches resident
- at most `16000` blamed lines resident in total

Evict least-recently-used file caches once either budget is exceeded.

## Invalidation Rules

Blame cache must be invalidated or suppressed when:

- the active file path changes
- the file is renamed or deleted
- the buffer becomes dirty
- the file is saved or reloaded
- git status changes between clean and non-clean for the file
- the repository `HEAD` changes
- the file crosses into or out of large-file mode

Dirty should suppress rendering immediately. The old cache may remain in memory, but it must not be
shown as if it still matches the current unsaved buffer.

## Rendering Notes

The renderer should receive immutable blame data that is already parsed and formatted.

Rendering should only do cheap work:

- look up a visible line in the current blame snapshot
- choose the muted blame color
- draw the clipped string in the reserved right-side column

Formatting details should stay out of the render loop as much as possible. Relative age strings and
summary truncation should be prepared when the blame snapshot is built or refreshed.

## Service Boundaries

The workspace should talk to blame through a project service boundary, not by shelling out directly
from workspace or editor files.

That keeps future backend changes possible:

- tighter subprocess handling while still using system git
- a shared git process helper instead of ad hoc `popen`
- eventual blame benchmarks or instrumentation without touching editor paint code

## Testing Plan

The first implementation should land with tests for:

- incremental blame parser coverage, including `boundary`, `previous`, repeated commits, unknown
  tags, and filename termination
- span-cache hits, misses, partial misses, and eviction
- invalidation on save, rename, delete, dirty-state changes, and `HEAD` changes
- workspace scheduling behavior for file open, tab switch, and viewport motion
- large-file-mode suppression
- narrow-pane suppression

Manual validation should include:

- a small clean tracked file
- a dirty buffer
- an untracked file
- a file that exceeds large-file thresholds
- quick wheel scrolling inside a warm cache window
- jumping far enough to require a new blame window
- branch switch or commit change while the file stays open

## Recommended Rollout

The safest rollout path is:

1. add the service, parser, cache, and tests without rendering anything yet
2. render blame only for eligible clean small files
3. validate scroll and typing latency on real repositories
4. only then consider broader cases such as working-tree content or deeper move detection

If the first pass shows measurable latency risk, keep the feature behind a temporary toggle until
the cache and invalidation behavior are proven.

## References

- Official `git blame` docs: <https://git-scm.com/docs/git-blame>
- Relevant options: `--incremental`, `--line-porcelain`, `-L`, `--encoding`, `--ignore-rev`,
  `--ignore-revs-file`, `-M`, `-C`, and `--contents`
