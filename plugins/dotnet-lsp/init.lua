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
    description = "Path to csharp-ls. Defaults to csharp-ls from PATH, falling back to the dotnet global tools directory (~/.dotnet/tools).",
  })
end

local function make_command(ctx, binary)
  -- csharp-ls speaks stdio with no extra flag and auto-discovers the .sln/.csproj
  -- from the project root. When the configured binary is the default name, defer
  -- resolution until first LSP use: prefer PATH, then the dotnet global tools dir.
  if binary ~= "csharp-ls" then
    return { binary }, binary
  end

  local command = {
    "sh",
    "-lc",
    [[
if command -v csharp-ls >/dev/null 2>&1; then
  exec csharp-ls "$@"
fi
tools_dir="${DOTNET_TOOLS:-$HOME/.dotnet/tools}"
if [ -x "$tools_dir/csharp-ls" ]; then
  exec "$tools_dir/csharp-ls" "$@"
fi
exec csharp-ls "$@"
    ]],
    "csharp-ls",
  }
  return command, "deferred resolver (PATH, ~/.dotnet/tools)"
end

return ide.plugin({
  id = "dotnet-lsp",

  setup = function(ctx)
    declare_settings(ctx)

    local binary = read_string(ctx, "binary", "csharp-ls")
    local command, description = make_command(ctx, binary)
    ctx.log("dotnet-lsp: using csharp-ls command: " .. description)

    ctx.lsp.add({
      id = "csharp-ls",
      language_ids = { "csharp" },
      command = command,
    })
  end,
})
