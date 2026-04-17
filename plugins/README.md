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

- `eslint`: runs ESLint on saved JavaScript and TypeScript files, then publishes diagnostics to the host Problems flow
- `bookmarks`: adds a small project-local bookmarks sidebar backed by `.microide/bookmarks.tsv`

After installing or editing a plugin, run `plugins-reload` inside microide.
