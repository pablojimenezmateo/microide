# Example Plugins

These are repo-owned dogfood plugins for the current manual Lua plugin runtime.

Install one by copying or symlinking its directory into either:

- `~/.config/microide/plugins/<plugin-id>/`
- `<project-root>/.microide/plugins/<plugin-id>/`

Example:

```bash
mkdir -p ~/.config/microide/plugins
ln -s /path/to/microide/plugins/eslint ~/.config/microide/plugins/eslint
```

Current examples:

- `eslint`: lints JavaScript, TypeScript, `tsconfig*.json`, and `jsconfig*.json` files on open and on save; publishes diagnostics to the host Problems and editor flows; `eslint.run` and `eslint.run-opened` force an immediate re-check
- `llm`: registers stdio-backed Codex chat and inline-completion agents through a persistent bridge that delegates requests to the local `codex exec` CLI in read-only mode, so auth, provider selection, and model execution stay aligned with the real Codex toolchain; configure the model with `llm.codex.model` (default `gpt-5.4`) and the binary path with `llm.codex.binary` (default `codex`); when the app PATH is sparse, the plugin also searches common user install locations such as `~/.nvm/versions/node/*/bin`; `llm-status` and `llm-logout` use the Codex CLI when available, while `llm-login` can still seed `~/.codex/auth.json` from inside microide; disable either registration with `llm.chat_enabled` or `llm.inline_enabled`
- `openai`: registers a direct OpenAI runtime-backed chat provider; configure `openai.base_url`, `openai.model`, and `openai.api_key`
- `anthropic`: registers a direct Anthropic Messages runtime-backed chat provider; configure `anthropic.base_url`, `anthropic.model`, and `anthropic.api_key`
- `deepseek`: registers a direct DeepSeek (OpenAI-compatible) runtime-backed chat provider; configure `deepseek.base_url`, `deepseek.model`, and `deepseek.api_key`
- `prettier`: registers stdin-based formatters for JavaScript, TypeScript, CSS, HTML, JSON, and Markdown; formats automatically on save via the host save pipeline; uses `node_modules/.bin/prettier` if present, otherwise `prettier` from PATH; override the binary path with project setting `prettier.binary`
- `typescript-lsp`: registers TypeScript LSP for the `typescript` language id (`.ts` and `.tsx` via current syntax detection); uses the project `node_modules/.bin/typescript-language-server` via an absolute path when present, otherwise defers server resolution until first LSP use and tries PATH/global binary, npm global bin, yarn global bin, then `npx --yes typescript-language-server --stdio`; the language-server process now launches from the project root so standard workspace-local TypeScript resolution stays aligned with typical VS Code setup; optional project setting is `typescript_lsp.binary`

TypeScript LSP setup:

```bash
npm install -D typescript typescript-language-server
```

After installing or editing a plugin, run `plugins-reload` inside microide.
