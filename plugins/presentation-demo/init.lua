local ide = require("microide")

-- Dogfood example for the Phase D presentation surfaces. All three are
-- host-owned and data-only (no network, no subprocess), so this plugin can be
-- copied into ~/.config/microide/plugins to exercise the surfaces:
--
--   * THEMES: `ctx.themes.add` contributes a colour scheme as a highlight-group
--     style map. The host derives a full theme (with contrast correction) the
--     same way it loads a `.microide` file, and the scheme appears in the
--     colorscheme picker as "presentation.demo.midnight".
--   * FILE ICONS: `ctx.file_icons.add` maps extensions / filenames to a built-in
--     gutter-icon shape + colour, drawn in the file-tree leading slot.
--   * RICH STATUS ITEMS: `ctx.status.add` items gain an icon, a tone-tinted
--     background, a click command, and an optional progress bar; `ctx.status.update`
--     mutates them live.

return ide.plugin({
  id = "presentation.demo",
  setup = function(ctx)
    -- A small dark theme. "default" sets the base fg,bg; the rest recolour
    -- individual highlight groups.
    ctx.themes.add({
      id = "midnight",
      label = "Midnight (demo)",
      colors = {
        ["default"] = "#cdd6f4,#11111b",
        comment = "#6c7086",
        statement = "#89b4fa",
        type = "#f9e2af",
        ["constant.string"] = "#a6e3a1",
        constant = "#fab387",
        preproc = "#cba6f7",
      },
    })

    -- File-type icons. Extension rules cover types; a filename rule covers a
    -- specific well-known file. Shapes come from the built-in vocabulary
    -- (dot/circle/diamond/triangle/bookmark/check/dash/square).
    ctx.file_icons.add({
      id = "demo",
      rules = {
        { ext = "csv", icon = "diamond", color = "#a6e3a1" },
        { ext = "log", icon = "dash", color = "#6c7086" },
        { ext = "lua", icon = "dot", color = "#89b4fa" },
        { name = "Makefile", icon = "square", color = "#f9e2af" },
      },
    })

    -- A rich status item: icon + tone + click command + progress bar. The
    -- command toggles the progress between two values so the bar animates on click.
    local progress = 0.25
    ctx.status.add({
      id = "indexer",
      text = "Indexing",
      tooltip = "Demo background task — click to advance",
      icon = "circle",
      tone = "info",
      command = "presentation.demo.tick",
      progress = progress,
      alignment = "right",
      priority = 50,
    })

    ctx.commands.add("presentation.demo.tick", function()
      progress = progress >= 1.0 and 0.0 or progress + 0.25
      ctx.status.update("indexer", {
        text = progress >= 1.0 and "Indexed" or "Indexing",
        tone = progress >= 1.0 and "default" or "info",
        progress = progress,
      })
    end)
  end,
})
