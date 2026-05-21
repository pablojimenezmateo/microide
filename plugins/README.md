# Example Plugins

These are repo-owned dogfood plugins for the current manual Lua plugin runtime.

Install one by copying or symlinking its directory into:

- `~/.config/microide/plugins/<plugin-id>/`

Example:

```bash
mkdir -p ~/.config/microide/plugins
ln -s /path/to/microide/plugins/eslint ~/.config/microide/plugins/eslint
```

Project-local plugin directories such as `<project-root>/.microide/plugins/` are not loaded.

Current examples:

- `eslint`: lints JavaScript, TypeScript, `tsconfig*.json`, and `jsconfig*.json` files on open and on save; publishes diagnostics to the host Problems and editor flows; `eslint.run` and `eslint.run-opened` force an immediate re-check
- `prettier`: registers stdin-based formatters for JavaScript, TypeScript, CSS, HTML, JSON, and Markdown; formats automatically on save via the host save pipeline; uses `node_modules/.bin/prettier` if present, otherwise `prettier` from PATH; override the binary path with project setting `prettier.binary`
- `typescript-lsp`: registers TypeScript LSP for the `typescript` language id (`.ts` and `.tsx` via current syntax detection); uses the project `node_modules/.bin/typescript-language-server` via an absolute path when present, otherwise defers server resolution until first LSP use and tries PATH/global binary, npm global bin, yarn global bin, then `npx --yes typescript-language-server --stdio`; the language-server process now launches from the project root so standard workspace-local TypeScript resolution stays aligned with typical VS Code setup; optional project setting is `typescript_lsp.binary`

TypeScript LSP setup:

```bash
npm install -D typescript typescript-language-server
```

After installing or editing a plugin, run `plugins-reload` inside microide.
