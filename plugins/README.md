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

- `eslint`: tracks JavaScript and TypeScript files opened in the current session, lints the ones dirtied in-session on save, and publishes diagnostics to the host Problems and editor flows
- `llm`: registers stdio-backed chat and inline-completion agents; defaults to `demo` provider (no credentials needed); set `llm.provider` to `claude` or `openai` in user settings and export `ANTHROPIC_API_KEY` or `OPENAI_API_KEY`; configure models via `llm.claude.model` (default `claude-sonnet-4-6`) or `llm.openai.model` (default `gpt-4o`); override either command directly via `llm.chat_command` or `llm.inline_command`
- `prettier`: registers stdin-based formatters for JavaScript, TypeScript, CSS, HTML, JSON, and Markdown; formats automatically on save via the host save pipeline; uses `node_modules/.bin/prettier` if present, otherwise `prettier` from PATH; override the binary path with project setting `prettier.binary`
- `typescript-lsp`: registers TypeScript LSP for the `typescript` language id (`.ts` and `.tsx` via current syntax detection); uses project `node_modules/.bin/typescript-language-server` directly when present, otherwise defers server resolution until first LSP use and tries PATH/global binary, npm global bin, yarn global bin, then `npx --yes typescript-language-server --stdio`; optional project settings are `typescript_lsp.binary` and `typescript_lsp.tsserver_path`

TypeScript LSP setup:

```bash
npm install -D typescript typescript-language-server
```

After installing or editing a plugin, run `plugins-reload` inside microide.
