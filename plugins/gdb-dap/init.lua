local ide = require("microide")

-- Contributes gdb's built-in DAP server (`gdb --interpreter=dap`, gdb >= 14) as a
-- debug adapter for native code (C, C++, Rust, ...). The host spawns gdb as a
-- subprocess, so the plugin needs the process-exec capability.
--
-- Adapter-only by design: there is no hardcoded launch config. Point a session at
-- a binary with the `debug-run` command (control channel or command palette),
-- e.g. `debug-run ./build/app`, or define a project launch config of type "gdb".
return ide.plugin({
  id = "gdb-dap",

  capabilities = {
    process = { exec = true },
  },

  setup = function(ctx)
    ctx.debug.add({
      id = "gdb",
      type = "gdb",
      command = { "gdb", "--interpreter=dap" },
    })
  end,
})
