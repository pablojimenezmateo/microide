local ide = require("microide")

local function trim(text)
  if type(text) ~= "string" then return "" end
  return (text:gsub("^%s+", ""):gsub("%s+$", ""))
end

local function read_string(ctx, suffix, fallback)
  local v = trim(ctx.settings.get("typescript_lsp." .. suffix))
  return v ~= "" and v or fallback
end

local function make_command(ctx, setting_binary, tsserver_path)
  local command
  local description

  if setting_binary ~= "typescript-language-server" then
    command = { setting_binary, "--stdio" }
    description = setting_binary
  elseif ctx.files.exists("node_modules/.bin/typescript-language-server") then
    command = { "node_modules/.bin/typescript-language-server", "--stdio" }
    description = "node_modules/.bin/typescript-language-server"
  else
    command = {
      "sh",
      "-lc",
      [[
if command -v typescript-language-server >/dev/null 2>&1; then
  exec typescript-language-server "$@"
fi
npm_prefix=$(npm prefix -g 2>/dev/null)
if [ -n "$npm_prefix" ] && [ -x "$npm_prefix/bin/typescript-language-server" ]; then
  exec "$npm_prefix/bin/typescript-language-server" "$@"
fi
yarn_bin=$(yarn global bin 2>/dev/null)
if [ -n "$yarn_bin" ] && [ -x "$yarn_bin/typescript-language-server" ]; then
  exec "$yarn_bin/typescript-language-server" "$@"
fi
exec npx --yes typescript-language-server "$@"
      ]],
      "typescript-language-server",
      "--stdio",
    }
    description = "deferred resolver (PATH, npm global, yarn global, npx fallback)"
  end

  if tsserver_path ~= "" then
    table.insert(command, "--tsserver-path")
    table.insert(command, tsserver_path)
  end

  return command, description
end

local function declare_settings(ctx)
  ctx.settings.declare({
    id = "binary",
    type = "string",
    default = "typescript-language-server",
    scope = "project",
    label = "TypeScript LSP Binary",
    description = "Path to typescript-language-server. Defaults to node_modules/.bin/typescript-language-server when present, otherwise typescript-language-server from PATH.",
  })
  ctx.settings.declare({
    id = "tsserver_path",
    type = "string",
    default = "",
    scope = "project",
    label = "TypeScript TSServer Path",
    description = "Optional path passed to --tsserver-path for typescript-language-server.",
  })
end

return ide.plugin({
  id = "typescript-lsp",

  setup = function(ctx)
    declare_settings(ctx)

    local binary = read_string(ctx, "binary", "typescript-language-server")
    local tsserver_path = read_string(ctx, "tsserver_path", "")
    local command, description = make_command(ctx, binary, tsserver_path)
    ctx.log("typescript-lsp: using command: " .. description)

    ctx.lsp.add({
      id = "typescript",
      language_id = "typescript",
      command = command,
    })
  end,
})
