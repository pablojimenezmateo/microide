## Why

MicroIDE currently treats AI workflows as a first-class built-in pillar, but the product direction has changed to remove all AI/LLM functionality from the host. We need a coordinated removal plan now because AI behavior is cross-cutting (UI, runtime, plugin APIs, persistence, build dependencies, and specs), and partial removal would leave contradictory contracts and dead code.

## What Changes

- **BREAKING** Remove all built-in AI/LLM product surfaces: chat sidebar UI, chat composer, inline ghost-text completion, AI provider picker/overlay, AI status-bar segment, and related commands/actions/menu entries.
- **BREAKING** Remove AI runtime and provider bridge stack: `AiProviderRuntime*`, provider bridge executable and protocol path, provider auth/model refresh flows, and AI context assembly.
- **BREAKING** Remove MCP tool execution paths that are AI-workflow owned, including chat-driven MCP invocation and approval UX/state.
- **BREAKING** Remove plugin AI extension surfaces and bundled AI plugins: contributed AI providers/external agents/MCP tool registrations and in-repo `openai`, `anthropic`, `deepseek`, and `llm` plugin packages.
- **BREAKING** Remove AI persistence payloads and migration paths for conversation/provider artifacts from workspace/session state.
- **BREAKING** Remove AI-related build and packaging dependencies, including `libcurl` and `microide_provider_bridge`, plus CI/package script wiring.
- Replace AI-specific performance/scenario coverage with non-AI equivalents and remove obsolete AI/chat perf baseline assets.
- Update documentation and contracts to remove AI-first positioning and eliminate stale references to AI/LLM capabilities.

## Capabilities

### New Capabilities

- `non-ai-product-scope`: define the durable product contract after removing built-in AI/LLM capabilities.

### Modified Capabilities

- `product-vision`: remove first-class AI workflow requirements and align the mission/pillars with a non-AI product scope.
- `ai-workflows`: retire host-owned chat, inline completion, MCP, and provider runtime requirements.
- `plugin-ai-provider-runtime`: retire plugin AI provider/runtime contribution requirements and related host integration guarantees.
- `workspace-architecture`: remove AI runtime services and AI-centric workflow scenarios from required architecture contracts.
- `persisted-state-format`: remove or explicitly deprecate AI conversation/provider records and define safe upgrade behavior.
- `shared-edit-primitives`: remove the chat composer multiline exception once the chat surface is removed.
- `settings-overlay-surface`: remove AI provider picker and AI-provider-specific responsive behaviors.
- `responsive-shell-layout`: remove chat-sidebar-specific compact layout guarantees.
- `workspace-status-bar`: remove AI provider/model status segment requirements.
- `performance-budgets`: remove AI-specific scheduling/scenario requirements and replace with non-AI equivalents.
- `performance-harness`: remove/replace the `chat_pane_long_transcript` required scenario.
- `lazy-gitignore-catalog`: remove AI-context-specific gitignore traversal requirements.
- `host-platform-support`: remove AI workflows from first-class platform support requirements.

## Impact

- Affected systems: workspace shell UI/render/input coordinators, runtime/event orchestration, plugin host/runtime seams, persistence codecs, command/action registries, and perf harness scenarios.
- Affected build/deps: root CMake targets, bridge target wiring, packaging scripts, and CI dependency setup (including `libcurl`).
- Affected tests: AI/chat/plugin integration tests, persistence round-trip tests for chat/provider records, status/settings/sidebar behavior tests, and chat perf baselines.
- User-visible impact: removal of all built-in AI/LLM features and related commands/configuration in favor of a non-AI IDE surface.
