## ADDED Requirements

### Requirement: SDL Event Loop Parks On Wait At True Idle

When MicroIDE has no pending user input, no active background work, and no animation other than caret blink, the application SHALL call `SDL_WaitEvent` (infinite wait) rather than `SDL_PollEvent` in a zero-delay loop. The application SHALL NOT produce unnecessary SDL wake events or scheduled zero-delay timeouts during true idle.

#### Scenario: Idle application consumes near-zero CPU
- **WHEN** MicroIDE has a project open, the user has not interacted for 30 seconds, and no background task (git, search, file-index, LSP) is in flight
- **THEN** the process SHALL consume effectively no CPU time as reported by `top` or equivalent, and the event loop SHALL be parked on `SDL_WaitEvent`

#### Scenario: Background task completion wakes the event loop
- **WHEN** a background task (git status, search batch, file-index update) completes while the event loop is parked
- **THEN** the background thread SHALL post an SDL user event to wake the event loop; the next frame SHALL be prepared and rendered promptly after the wake

### Requirement: Caret-Only Animation Uses Blink-Period Timeout

When the only pending animation is a caret blink (no user input, no background work, no other animated surface) the application SHALL use `SDL_WaitEventTimeout` with the remaining time until the next caret blink transition as the sole timeout. This allows the caret to animate correctly without keeping the event loop in a zero-delay spin.

#### Scenario: Caret blinks at the correct interval without a spin loop
- **WHEN** the user has stopped typing and the only active animation is the caret blink
- **THEN** the event loop SHALL use `SDL_WaitEventTimeout(caret_remaining_ms)` so that caret transitions occur at the configured blink interval and the process is parked between transitions

#### Scenario: User input immediately interrupts the caret-wait timeout
- **WHEN** the event loop is waiting on `SDL_WaitEventTimeout` for the next caret blink
- **THEN** any key press, mouse event, or SDL user event SHALL wake the loop immediately without waiting for the timeout to expire

### Requirement: Idle Hint Is Derived From FrameToken And Respected By Application

`PrepareFrameOnce` SHALL return an `IdleHint` value (`Full`, `CaretOnly`, or `Idle`) derived from the current in-flight background task count, pending input state, and active animation set. The `Application` event loop SHALL use this hint to select the appropriate SDL wait strategy for the next iteration.

#### Scenario: IdleHint transitions from Full to Idle as activity settles
- **WHEN** the user stops typing, all background tasks complete, and the caret is not active
- **THEN** the `IdleHint` SHALL transition through `CaretOnly` (if a caret was active) and eventually to `Idle` within the caret-blink cycle after the last activity, and the event loop SHALL enter `SDL_WaitEvent`

#### Scenario: In-flight background task count never goes negative
- **WHEN** any background service decrements the in-flight task counter
- **THEN** the counter SHALL never reach a value below zero; an assertion SHALL fire in ASAN builds if this invariant is violated
