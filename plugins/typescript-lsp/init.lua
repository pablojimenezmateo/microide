local ide = require("microide")

local function trim(text)
  if type(text) ~= "string" then return "" end
  return (text:gsub("^%s+", ""):gsub("%s+$", ""))
end

local function read_string(ctx, suffix, fallback)
  local v = trim(ctx.settings.get("typescript_lsp." .. suffix))
  return v ~= "" and v or fallback
end

local function project_root(ctx)
  local root = ctx.workspace.project_root()
  if type(root) == "string" and root ~= "" then
    return root
  end
  return nil
end

local function project_relative_path(ctx, relative_path)
  local root = project_root(ctx)
  if root == nil then
    return relative_path
  end
  return root .. "/" .. relative_path
end

local function make_command(ctx, setting_binary)
  local command
  local description

  if setting_binary ~= "typescript-language-server" then
    command = { setting_binary, "--stdio" }
    description = setting_binary
  elseif ctx.files.exists("node_modules/.bin/typescript-language-server") then
    command = { project_relative_path(ctx, "node_modules/.bin/typescript-language-server"), "--stdio" }
    description = "project node_modules/.bin/typescript-language-server"
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
end

return ide.plugin({
  id = "typescript-lsp",

  setup = function(ctx)
    declare_settings(ctx)

    local binary = read_string(ctx, "binary", "typescript-language-server")
    local command, description = make_command(ctx, binary)
    ctx.log("typescript-lsp: using command: " .. description)

    ctx.lsp.add({
      id = "typescript",
      language_id = "typescript",
      command = command,
    })
  end,
})
