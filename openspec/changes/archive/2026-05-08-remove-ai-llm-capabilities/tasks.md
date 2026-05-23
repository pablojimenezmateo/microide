## 1. Contract and scope alignment

- [x] 1.1 Update `openspec/specs/product-vision/spec.md` to remove AI-first thesis requirements and align built-in workflow definitions with non-AI scope.
- [x] 1.2 Retire `openspec/specs/ai-workflows/spec.md` and `openspec/specs/plugin-ai-provider-runtime/spec.md` requirements in the implementation branch.
- [x] 1.3 Update dependent OpenSpec capabilities (`workspace-architecture`, `persisted-state-format`, `shared-edit-primitives`, `settings-overlay-surface`, `responsive-shell-layout`, `workspace-status-bar`, `performance-budgets`, `performance-harness`, `lazy-gitignore-catalog`, `host-platform-support`) to remove AI assumptions.

## 2. Remove host AI/LLM runtime and UI surfaces

- [x] 2.1 Remove chat sidebar/composer/transcript state, rendering, and input routing paths from workspace shell/coordinators.
- [x] 2.2 Remove inline completion registries, request/accept/dismiss flows, and ghost-text rendering in editor paths.
- [x] 2.3 Remove AI provider auth/picker/status integration and related commands/actions/menu entries.
- [x] 2.4 Remove MCP invocation/approval flows that are AI-workflow-owned, including chat-driven tool execution paths.

## 3. Remove provider bridge and dependency chain

- [x] 3.1 Remove `AiProviderRuntime*`, provider bridge manager/protocol wiring, and runtime event processing paths.
- [x] 3.2 Remove bridge executable sources/targets (`ProviderBridgeMain`, `CloudProviderBridge`) and associated startup/test wiring.
- [x] 3.3 Remove `libcurl` and AI bridge dependency plumbing from `CMakeLists.txt`, CI workflows, and packaging scripts.

## 4. Remove AI plugin surfaces and bundled plugins

- [x] 4.1 Remove plugin host contribution seams for AI providers, external agents, and AI-owned MCP integration.
- [x] 4.2 Remove Lua interop modules and parser paths used only for AI provider/agent/tool registration.
- [x] 4.3 Remove bundled AI plugins (`plugins/openai`, `plugins/anthropic`, `plugins/deepseek`, `plugins/llm`) and any runtime registration references.

## 5. Persistence and migration safety

- [x] 5.1 Remove AI conversation/provider write paths from persistence codecs and session/state save flows.
- [x] 5.2 Ensure loaders tolerate legacy AI records without failure and ignore retired AI payloads during runtime restore.
- [x] 5.3 Add/adjust persistence regression tests to validate legacy-load compatibility and AI-record omission on save.

## 6. Tests, perf harness, and docs closure

- [x] 6.1 Remove or rewrite AI/chat/plugin integration tests and test-access seams tied to removed features.
- [x] 6.2 Replace `chat_pane_long_transcript` perf scenario/baseline with a non-AI required scenario and update harness assertions.
- [x] 6.3 Update durable docs (`AGENTS.md`, `CLAUDE.md`, `dev-docs/project/implementation-guide.md`, `dev-docs/project/active-work.md`, `ROADMAP.md`, and relevant guidelines) to remove AI references and align with updated specs.
- [x] 6.4 Run targeted build/tests for touched subsystems and record validation evidence for the change.
