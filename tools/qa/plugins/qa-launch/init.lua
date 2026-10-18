-- A QA-only plugin (tools/qa): a launch configuration for the taskflow fixture's
-- binary, so the `debug-keys` flow can start a session with F5 as a user does.
-- Launch configurations come only from plugins (ctx.debug.addConfig); no bundled
-- plugin ships one for a particular program.
local ide = require("microide")

return ide.plugin({
  id = "qa-launch",
  capabilities = {
    process = { exec = true },
  },
  setup = function(ctx)
    ctx.debug.addConfig({
      id = "taskflow",
      name = "taskflow (QA)",
      type = "gdb",
      request = "launch",
      arguments = '{"program": "${workspaceFolder}/taskflow"}',
    })
  end,
})
