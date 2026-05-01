## ADDED Requirements

### Requirement: PCRE2 JIT Compilation Is Enabled And Cached Per Pattern

The search engine SHALL call `pcre2_jit_compile()` immediately after `pcre2_compile()` for every new pattern. Compiled patterns SHALL be stored in a bounded LRU cache keyed by `(pattern_string, flags)` so that repeated searches with the same pattern reuse the JIT-compiled form without recompilation. The cache SHALL hold at most 64 entries; excess entries SHALL be evicted by LRU order.

#### Scenario: Repeated search with the same pattern avoids recompilation
- **WHEN** the user initiates two consecutive searches with identical pattern strings and flags
- **THEN** the second search SHALL use the cached JIT-compiled pattern; `pcre2_compile()` and `pcre2_jit_compile()` SHALL each be called at most once for that pattern during the session

#### Scenario: JIT unavailability is handled gracefully
- **WHEN** `pcre2_jit_compile()` returns an error indicating JIT is unsupported on the current platform
- **THEN** the search engine SHALL log a one-time diagnostic at startup, fall back to interpreted PCRE2 execution, and continue operating correctly without error

#### Scenario: Cache is bounded and does not grow unboundedly
- **WHEN** the user issues more than 64 distinct search patterns in a session
- **THEN** the oldest cached patterns SHALL be evicted to stay within the 64-entry limit; no compiled pattern SHALL be leaked

### Requirement: Search Results Are Streamed To The UI Incrementally

The search worker thread SHALL deliver result batches to the UI before the full project corpus has been scanned. The UI SHALL render partial results on each frame that arrives after a wake event, without waiting for the worker to finish. The user SHALL see the first results within the time-to-first-result budget defined in `performance-budgets`.

#### Scenario: First results appear before full-corpus scan completes
- **WHEN** the user initiates a search on a project with more than 100 files
- **THEN** the first matching results SHALL appear in the UI within the time-to-first-result budget, even if the full scan has not yet completed

#### Scenario: UI renders partial results on each wake event
- **WHEN** the search worker posts a result batch via SDL wake event
- **THEN** the UI SHALL update the search result view on the next frame to display all results received so far, including those from previous batches

#### Scenario: Cancellation stops the worker at the next file boundary
- **WHEN** the user dismisses the search overlay or initiates a new search while a previous search is in flight
- **THEN** the previous search worker SHALL stop processing files at the next file boundary after the cancellation token is set, and SHALL NOT deliver further results to the UI

#### Scenario: Empty search results are handled correctly
- **WHEN** a search completes with no matches found across the entire corpus
- **THEN** the UI SHALL display the empty-results state after receiving the final wake event from the worker, with no partial-results artefacts visible
