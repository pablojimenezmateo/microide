local ide = require("microide")

-- Maps microide filetype IDs to prettier parser names.
-- Filetypes come from RuntimeSyntaxGenerated.cpp pattern matching.
local FORMATTERS = {
  { language_id = "javascript", parser = "babel" },
  { language_id = "typescript", parser = "typescript" },
  { language_id = "css",        parser = "css" },
  { language_id = "html",       parser = "html" },
  { language_id = "json",       parser = "json" },
  { language_id = "markdown",   parser = "markdown" },
}

local function trim(text)
  if type(text) ~= "string" then return "" end
  return (text:gsub("^%s+", ""):gsub("%s+$", ""))
end

local function read_string(ctx, suffix, fallback)
  local v = trim(ctx.settings.get("prettier." .. suffix))
  return v ~= "" and v or fallback
end

local function resolve_binary(ctx, setting_binary)
  if setting_binary ~= "prettier" then
    return setting_binary
  end
  if ctx.files.exists("node_modules/.bin/prettier") then
    return "node_modules/.bin/prettier"
  end
  return "prettier"
end

local function declare_settings(ctx)
  ctx.settings.declare({
    id = "binary",
    type = "string",
    default = "prettier",
    scope = "project",
    label = "Prettier Binary",
    description = "Path to the prettier binary. Defaults to node_modules/.bin/prettier if present, otherwise prettier from PATH.",
  })
end

return ide.plugin({
  id = "prettier",

  setup = function(ctx)
    declare_settings(ctx)

    local binary = resolve_binary(ctx, read_string(ctx, "binary", "prettier"))

    for _, spec in ipairs(FORMATTERS) do
      ctx.formatters.add({
        id = spec.language_id,
        language_id = spec.language_id,
        label = "Prettier",
        command = { binary, "--parser", spec.parser },
      })
    end
  end,
})
