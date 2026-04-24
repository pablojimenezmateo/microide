# ADR Guide

Purpose: record durable architecture decisions that outlive a single implementation slice.

## Quick Scan

- Use an ADR for long-lived policy, not change-local notes.
- Capture decisions about subsystem boundaries, host ownership, plugin extension limits, performance policy, persistence shape, or major integration strategy.
- Name files `<short-title>.md`.
- Start from `template.md`.

## Current ADRs

- [ADR-001: Plugin Contributions Flow Through Host-Owned Registries](plugin-contributions-host-owned-registries.md) — Accepted. Plugins extend narrow registries and services rather than reaching into `WorkspaceShell`.
- [ADR-002: Built-In IDE Workflows Remain Host-Owned](built-in-workflows-remain-host-owned.md) — Accepted. Core editor, compare, merge, search, git, and terminal flows stay built in while plugins extend around them.
