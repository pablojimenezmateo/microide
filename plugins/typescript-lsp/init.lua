local ide = require("microide")

local function trim(text)
  if type(text) ~= "string" then return "" end
  return (text:gsub("^%s+", ""):gsub("%s+$", ""))
end

local function read_string(ctx, suffix, fallback)
  local v = trim(ctx.settings.get("typescript_lsp." .. suffix))
  return v ~= "" and v or fallback
end

-- The server is resolved where and when it starts (the host's FirstAvailableCommand),
-- so each project gets its own node_modules copy, a remote project the host's, and
-- nothing is ever downloaded: with neither found, the hint says what to install.
local function server_commands(binary)
  if binary ~= "typescript-language-server" then
    return { { binary, "--stdio" } }, binary
  end
  return {
    { "${workspaceFolder}/node_modules/.bin/typescript-language-server", "--stdio" },
    { "typescript-language-server", "--stdio" },
  }, "the project's node_modules/.bin, then PATH"
end

local INSTALL_HINT = "typescript-language-server not found: add it to the project " ..
  "(npm install --save-dev typescript-language-server typescript) or install it " ..
  "globally (npm install -g typescript-language-server typescript)"

local function declare_settings(ctx)
  ctx.settings.declare({
    id = "binary",
    type = "string",
    default = "typescript-language-server",
    scope = "project",
    label = "TypeScript LSP Binary",
    description = "Path to typescript-language-server. Defaults to the project's node_modules/.bin/typescript-language-server when present, otherwise typescript-language-server from PATH -- resolved where the server runs (the host, for a remote project). Nothing is downloaded.",
  })
end

return ide.plugin({
  id = "typescript-lsp",

  -- Contributes a language server, which the host launches as a subprocess, so it needs the
  -- process execution capability. Filesystem access stays at the project-scoped default.
  capabilities = {
    process = { exec = true },
  },

  setup = function(ctx)
    declare_settings(ctx)

    local binary = read_string(ctx, "binary", "typescript-language-server")
    local commands, description = server_commands(binary)
    ctx.log("typescript-lsp: using " .. description)

    ctx.lsp.add({
      id = "typescript",
      language_ids = { "typescript" },
      commands = commands,
      install_hint = INSTALL_HINT,
    })
  end,
})
