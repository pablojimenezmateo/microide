# C++ Guide

Purpose: define the standard C++ implementation practices for `microide`.

## Quick Scan

- Prefer explicit ownership, RAII, and value semantics.
- Use composition over inheritance unless there is a durable polymorphic boundary.
- Keep headers narrow and interfaces stable.
- Use standard-library vocabulary types to express intent.
- Make invariants obvious in types and constructors instead of scattering defensive checks everywhere.

## Ownership And Lifetime

- Default to stack values and direct members when lifetime is straightforward.
- Use `std::unique_ptr` for exclusive heap ownership.
- Use `std::shared_ptr` only when shared lifetime is required and explicit.
- Prefer references or pointers for non-owning access, and document lifetime expectations near the interface.
- Use RAII to tie resource acquisition and release to object lifetime, especially for SDL handles, file descriptors, subprocess state, and temporary filesystem resources.

## Classes Versus Helpers

- Use a class or struct when a type owns state, enforces invariants, or represents a durable concept in the system.
- Use a plain helper function for small deterministic transformations with no owned state.
- Avoid classes that only group unrelated static helpers.
- Avoid inheritance-heavy hierarchies for routine application logic; focused value types and helper functions are usually easier to maintain.

## Header And Source Boundaries

- Keep headers lean. Include only what the interface needs.
- Prefer forward declarations where they keep dependencies obvious and safe.
- Put implementation details in `.cpp` files unless a header-only definition is required.
- Do not expose subsystem internals just to save a small amount of wiring.
- If a header becomes difficult to read because it owns too many concerns, split the ownership boundary instead of piling on comments.

## Standard Vocabulary Types

- `std::string_view`:
  - Use for non-owning read-only string parameters when the callee does not need to store the data.
- `std::span`:
  - Use for non-owning ranges when contiguous data and explicit size both matter.
- `std::filesystem::path`:
  - Use at file and process boundaries instead of plain strings.
- `std::optional`:
  - Use when a value is legitimately absent and that absence is part of the interface contract.
- `std::unique_ptr`:
  - Use when ownership transfer or stable heap allocation is required.

Choose the narrowest type that expresses the real contract. Do not force callers through needless conversions.

## Error Handling And Invariants

- Keep programmer errors and runtime failures conceptually separate.
- Enforce invariants where state enters the system or where a type is constructed.
- Prefer interfaces that make invalid states hard to represent.
- Return structured failure information for operational errors that callers are expected to handle.
- Use assertions for impossible internal states, not as a substitute for input validation at real boundaries.

Parsing policy:

- Prefer non-throwing parse helpers in `src/util/Parse.{h,cpp}` over exception-driven numeric parsing.
- Do not introduce `try`/`catch` wrappers around `std::stoi`, `std::stoll`, `std::stoull`, `std::stof`, or `std::stod`.

## Dependency And State Management

- Prefer explicit dependencies passed to constructors or functions over hidden global access.
- Keep mutable state local to the type that truly owns it.
- When multiple parts of the shell need shared behavior, create a focused helper or service instead of a catch-all utility singleton.
- If a type becomes hard to test, it usually owns too much.
