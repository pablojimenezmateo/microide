-- A QA-only plugin (tools/qa): a save participant and a formatter for `.qasave`
-- files, so the `save-pipeline` flow drives the save-time hooks no bundled
-- plugin exercises without external tools (prettier, eslint).
local ide = require("microide")

return ide.plugin({
  id = "qa-save",
  capabilities = { process = { exec = true } },
  setup = function(ctx)
    -- Whole-document transform at save time: every "qa-todo" becomes "qa-done".
    ctx.save_participants.add("qa-done", function(buffer)
      return { text = (buffer.text:gsub("qa%-todo", "qa-done")) }
    end)
    -- The formatter format-on-save runs: upper-cases the buffer through `tr`.
    -- With $XDG_RUNTIME_DIR/qa-slow-format present the formatter takes two
    -- seconds: the `save-wait` flow needs a save that is still formatting when
    -- the close, quit or project close that asked for it waits on it.
    ctx.formatters.add({
      id = "qa-upper",
      language_id = "qasave",
      label = "QA Uppercase",
      command = { "sh", "-c", "[ -f \"$XDG_RUNTIME_DIR/qa-slow-format\" ] && sleep 2; tr '[:lower:]' '[:upper:]'" },
    })
  end,
})
