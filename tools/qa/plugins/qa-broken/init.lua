-- QA-only: a plugin that gets its registrations wrong, for tools/qa's
-- `plugin-errors` flow. Each bad registration runs under pcall and its refusal is
-- written to qa-broken-errors.txt (one "name: message" line each), so the flow can
-- check the host refuses each with a message that says what is wrong and keeps
-- the plugin's good contributions. The last one is NOT caught: a setup that
-- raises must show the user which plugin failed and why. The registration
-- parsers' refusals ran at under half in the coverage build (TD-2026-10-18-364).
local ide = require("microide")

return ide.plugin({
  id = "qa-broken",
  capabilities = { process = { exec = true } },

  setup = function(ctx)
    local lines = {}
    local function attempt(name, fn)
      local ok, err = pcall(fn)
      lines[#lines + 1] = name .. ": " .. (ok and "accepted" or tostring(err))
    end
    ctx.commands.add("qa.broken.ok", function() return "still here" end)
    attempt("completion", function() ctx.completion.add({ id = "c", provide = function() return {} end }) end)
    attempt("menu", function() ctx.menus.add({ id = "m", menu = "Toolz", action = "qa.broken.ok", label = "QA" }) end)
    attempt("keybinding", function() ctx.keybindings.add({ id = "k", action = "qa.broken.ok" }) end)
    attempt("setting", function() ctx.settings.declare({ id = "s" }) end)
    attempt("status", function() ctx.status.add({ text = "no id" }) end)
    attempt("formatter", function()
      ctx.formatters.add({ id = "f", language_id = "qaapi", label = "F", command = {} })
    end)
    attempt("code_action", function() ctx.code_actions.add({ id = "a", provide = function() return {} end }) end)
    ctx.files.write_text("qa-broken-errors.txt", table.concat(lines, "\n") .. "\n")
    -- Uncaught: the plugin's setup fails here.
    ctx.menus.add({ id = "m2", menu = "Nowhere", action = "qa.broken.ok", label = "QA 2" })
  end,
})
