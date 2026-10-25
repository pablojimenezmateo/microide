local ide = require("microide")

local function trim(text)
  if type(text) ~= "string" then return "" end
  return (text:gsub("^%s+", ""):gsub("%s+$", ""))
end

local function read_string(ctx, suffix, fallback)
  local v = trim(ctx.settings.get("dotnet_lsp." .. suffix))
  return v ~= "" and v or fallback
end

local function declare_settings(ctx)
  ctx.settings.declare({
    id = "binary",
    type = "string",
    default = "csharp-ls",
    scope = "project",
    label = "C# Language Server Binary",
    description = "Path to csharp-ls. Defaults to csharp-ls from PATH, falling back to the dotnet global tools directory (~/.dotnet/tools) -- resolved where the server runs.",
  })
end

-- Resolved where and when the server starts (the host's FirstAvailableCommand):
-- PATH first, then the dotnet global tools directory.
local function server_commands(binary)
  if binary ~= "csharp-ls" then
    return { { binary } }, binary
  end
  return { { "csharp-ls" }, { "~/.dotnet/tools/csharp-ls" } }, "PATH, then ~/.dotnet/tools"
end

local INSTALL_HINT = "csharp-ls not found: install it with `dotnet tool install --global csharp-ls`"

return ide.plugin({
  id = "dotnet-lsp",

  -- Contributes the csharp-ls language server, which the host launches as a subprocess, so it
  -- needs the process execution capability. Filesystem access stays at the project-scoped default.
  capabilities = {
    process = { exec = true },
  },

  setup = function(ctx)
    declare_settings(ctx)

    local binary = read_string(ctx, "binary", "csharp-ls")
    local commands, description = server_commands(binary)
    ctx.log("dotnet-lsp: using csharp-ls from " .. description)

    ctx.lsp.add({
      id = "csharp-ls",
      language_ids = { "csharp" },
      commands = commands,
      install_hint = INSTALL_HINT,
    })
  end,
})
