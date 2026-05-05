local ide = require("microide")

local function trim(text)
  return (text or ""):match("^%s*(.-)%s*$")
end

local function read_string(ctx, key, default_value)
  local value = trim(ctx.settings.get("anthropic." .. key))
  if value == "" then
    return default_value
  end
  return value
end

return ide.plugin({
  id = "anthropic",

  setup = function(ctx)
    ctx.settings.declare({
      id = "base_url",
      type = "string",
      default = "https://api.anthropic.com",
      scope = "user",
      label = "Anthropic Base URL",
      description = "Base URL for the Anthropic API.",
    })
    ctx.settings.declare({
      id = "model",
      type = "string",
      default = "claude-sonnet-4-6",
      scope = "user",
      label = "Anthropic Model",
      description = "Default Anthropic model ID for chat requests.",
    })
    ctx.settings.declare({
      id = "api_key",
      type = "string",
      default = "",
      scope = "user",
      label = "Anthropic API Key",
      description = "API key used for Anthropic chat requests.",
    })

    local base_url = read_string(ctx, "base_url", "https://api.anthropic.com")
    local model = read_string(ctx, "model", "claude-sonnet-4-6")

    ctx.ai_providers.add({
      id = "chat",
      label = "Anthropic",
      type = "cloud",
      models = { model },
      runtime = "anthropic_messages",
      base_url = base_url,
      default_model = model,
    })
  end,
})
