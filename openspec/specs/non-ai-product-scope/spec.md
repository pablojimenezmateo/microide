# non-ai-product-scope Specification

## Purpose
Define the non-AI product boundary for the host application. MicroIDE must not ship built-in AI or
LLM workflows, provider bridges, ghost-text completion, chat surfaces, or required AI transport
dependencies; extension points may remain only as general plugin infrastructure.

## Requirements
### Requirement: Built-In AI And LLM Features Are Not Shipped
The system SHALL not ship host-owned AI/LLM capabilities, including chat assistants, ghost-text inline completion, provider runtime bridges, or AI-provider configuration UX.

This prohibition is on **host-owned AI**: a built-in provider, model picker, chat surface, or completion engine. It does NOT prohibit a generic, plugin-driven ghost-text *rendering* seam (`ctx.editor.set_ghost_text`) that paints plugin-supplied text with no host provider/model/chat — that is "general plugin infrastructure" (see Purpose) and the host owns only the rendering and accept/dismiss lifecycle, not any AI workflow.

#### Scenario: User inspects built-in workflows
- **WHEN** product workflows and host surfaces are enumerated
- **THEN** no built-in AI/LLM workflow is present

### Requirement: AI/LLM Dependencies Are Not Required For Build
The system SHALL build and run without AI/LLM transport dependencies and bridge executables previously used for hosted model/provider integration.

#### Scenario: Build configuration is evaluated
- **WHEN** required third-party dependencies are resolved for standard builds
- **THEN** AI/LLM-specific HTTP bridge dependencies are not required
