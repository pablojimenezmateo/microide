## 1. Establish Host Service Seams

- [x] 1.1 Audit the current Linux-only and generic POSIX paths in app directories, subprocess launch, terminal session lifecycle, file watching, file operations, and host integrations, and map each one to an explicit host-owned service boundary.
- [x] 1.2 Refactor the existing Linux-first implementations so workspace and project code depend on platform service contracts instead of direct Linux or POSIX behavior.
- [x] 1.3 Keep Linux working through the new seams with focused regression coverage for directories, process launch, terminal basics, file watching, and trash behavior.

## 2. Add macOS And Windows Backends

- [x] 2.1 Implement macOS-correct and Windows-correct app directory resolution, host integration actions, and recycle-bin or trash behavior behind the new platform services.
- [ ] 2.2 Split terminal session lifecycle from terminal rendering and add macOS and Windows backend support for terminal and subprocess launch, resize, shutdown, and error handling.
- [ ] 2.3 Add native or host-appropriate file-watcher backends for macOS and Windows, keeping polling only as an explicit fallback path rather than the supported-host default.

## 3. Package, Validate, And Document Supported Hosts

- [x] 3.1 Add packaging and launch support for macOS app bundles and Windows desktop builds, including runtime asset discovery and documented local bring-up steps.
- [x] 3.2 Add targeted automated validation and CI coverage for Linux, macOS, and Windows host-facing workflows affected by the new platform services.
- [x] 3.3 Update durable docs such as `docs/implementation-guide.md`, `docs/active-work.md`, and platform support notes so the supported-host contract, known gaps, and rollout plan stay aligned with the shipped architecture.
