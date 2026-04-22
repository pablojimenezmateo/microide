local ide = require("microide")

local function trim(text)
  if type(text) ~= "string" then return "" end
  return (text:gsub("^%s+", ""):gsub("%s+$", ""))
end

local function run_stdout(ctx, argv)
  local result = ctx.process.run(argv, { cwd = "." })
  if type(result) ~= "table" or tonumber(result.exit_code) ~= 0 then
    return ""
  end
  return trim(result.stdout or "")
end

local function read_string(ctx, suffix, fallback)
  local v = trim(ctx.settings.get("typescript_lsp." .. suffix))
  return v ~= "" and v or fallback
end

local function resolve_binary(ctx, setting_binary)
  if setting_binary ~= "typescript-language-server" then
    return setting_binary
  end
  if ctx.files.exists("node_modules/.bin/typescript-language-server") then
    return "node_modules/.bin/typescript-language-server"
  end

  local path_binary = run_stdout(ctx, { "sh", "-lc", "command -v typescript-language-server" })
  if path_binary ~= "" then
    return path_binary
  end

  local npm_prefix = run_stdout(ctx, { "npm", "prefix", "-g" })
  if npm_prefix ~= "" then
    local npm_global_binary = npm_prefix .. "/bin/typescript-language-server"
    if ctx.files.exists(npm_global_binary) then
      return npm_global_binary
    end
  end

  local yarn_global_bin = run_stdout(ctx, { "yarn", "global", "bin" })
  if yarn_global_bin ~= "" then
    local yarn_global_binary = yarn_global_bin .. "/typescript-language-server"
    if ctx.files.exists(yarn_global_binary) then
      return yarn_global_binary
    end
  end

  return ""
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

    local binary = resolve_binary(ctx, read_string(ctx, "binary", "typescript-language-server"))
    local tsserver_path = read_string(ctx, "tsserver_path", "")
    local command
    if binary ~= "" then
      command = { binary, "--stdio" }
      ctx.log("typescript-lsp: using binary: " .. binary)
    else
      command = { "npx", "--yes", "typescript-language-server", "--stdio" }
      ctx.log("typescript-lsp: using npx fallback for typescript-language-server")
    end

    if tsserver_path ~= "" then
      table.insert(command, "--tsserver-path")
      table.insert(command, tsserver_path)
    end

    ctx.lsp.add({
      id = "typescript",
      language_id = "typescript",
      command = command,
    })
  end,
})
