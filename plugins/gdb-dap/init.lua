local ide = require("microide")

-- Contributes gdb's built-in DAP server (`gdb --interpreter=dap`, gdb >= 14) as a
-- debug adapter for native code (C, C++, Rust, ...). The host spawns gdb as a
-- subprocess, so the plugin needs the process-exec capability.
--
-- Adapter-only by design: there is no hardcoded launch config. Point a session at
-- a binary with the `debug-run` command (control channel or command palette),
-- e.g. `debug-run ./build/app`, or define a project launch config of type "gdb".
--
-- Reverse / time-travel debugging:
--   * type "gdb"    — launch normally, then run `record` in the debug console to
--                     start gdb's built-in process record/replay. gdb then sends a
--                     DAP `capabilities` event turning on reverse execution, so the
--                     Reverse Continue / Step Back buttons appear.
--   * type "gdb-rr" — replay the most recent Mozilla rr recording (run `rr record
--                     ./build/app` once in a terminal first). rr launches gdb in DAP
--                     mode already attached to the replay target, which is reverse-
--                     capable from the first stop. Needs the `rr` binary on PATH.
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
    ctx.debug.add({
      id = "gdb-rr",
      type = "gdb-rr",
      -- rr drives gdb (`-d gdb`) and forwards `--interpreter=dap` to it (`-o ...`),
      -- wiring gdb to rr's replay server. With no trace dir it replays the latest
      -- recording under $_RR_TRACE_DIR (default ~/.local/share/rr/latest-trace).
      command = { "rr", "replay", "-d", "gdb", "-o", "--interpreter=dap" },
    })
  end,
})
