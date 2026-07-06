local ide = require("microide")

local function trim(text)
  if type(text) ~= "string" then return "" end
  return (text:gsub("^%s+", ""):gsub("%s+$", ""))
end

local function read_string(ctx, suffix, fallback)
  local v = trim(ctx.settings.get("cpp_lsp." .. suffix))
  return v ~= "" and v or fallback
end

local function read_bool(ctx, suffix, fallback)
  local v = trim(ctx.settings.get("cpp_lsp." .. suffix)):lower()
  if v == "" then return fallback end
  return v == "true" or v == "1" or v == "yes" or v == "on"
end

local function declare_settings(ctx)
  ctx.settings.declare({
    id = "binary",
    type = "string",
    default = "clangd",
    scope = "project",
    label = "clangd Binary",
    description = "Path to the clangd executable. Defaults to clangd from PATH.",
  })
  ctx.settings.declare({
    id = "background_index",
    type = "bool",
    default = "true",
    scope = "project",
    label = "clangd Background Index",
    description = "Build a background index of the whole project for cross-file features. Disable to lower CPU/memory at the cost of project-wide navigation.",
  })
end

local function make_command(ctx, binary, background_index)
  -- clangd is normally a single PATH binary; keep the command minimal.
  -- --clang-tidy and IWYU header insertion improve correctness with negligible
  -- idle cost; --background-index is the heavy footprint knob and is toggleable.
  local command = { binary, "--clang-tidy", "--header-insertion=iwyu" }
  if background_index then
    table.insert(command, "--background-index")
  end
  return command
end

return ide.plugin({
  id = "cpp-lsp",

  -- Contributes the clangd language server, which the host launches as a subprocess, so it needs
  -- the process execution capability. Filesystem access stays at the project-scoped default.
  capabilities = {
    process = { exec = true },
  },

  setup = function(ctx)
    declare_settings(ctx)

    local binary = read_string(ctx, "binary", "clangd")
    local background_index = read_bool(ctx, "background_index", true)
    local command = make_command(ctx, binary, background_index)
    ctx.log("cpp-lsp: using clangd command: " .. table.concat(command, " "))

    -- One clangd process serves C, C++, and Objective-C; the host shares a
    -- single subprocess across these language ids. clangd reads
    -- compile_commands.json from the project root automatically.
    ctx.lsp.add({
      id = "clangd",
      language_ids = { "c", "c++", "objective-c" },
      command = command,
      -- Lazy start (on first C/C++ file open) is the default: clangd inits in <1s
      -- and the host now engages the server on file open (didOpen + diagnostics +
      -- semantic tokens), so there is no "Starting..."/blank-decorations wait. Set
      -- eager_start = true only to overlap clangd's background index with browsing
      -- on projects large enough for indexing to matter (needs a compile DB).
      eager_start = false,
    })
  end,
})
