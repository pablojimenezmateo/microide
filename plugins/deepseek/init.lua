local ide = require("microide")

local function trim(text)
  return (text or ""):match("^%s*(.-)%s*$")
end

local function read_string(ctx, key, default_value)
  local value = trim(ctx.settings.get("deepseek." .. key))
  if value == "" then
    return default_value
  end
  return value
end

return ide.plugin({
  id = "deepseek",

  setup = function(ctx)
    ctx.settings.declare({
      id = "base_url",
      type = "string",
      default = "https://api.deepseek.com",
      scope = "user",
      label = "DeepSeek Base URL",
      description = "Base URL for the DeepSeek API.",
    })
    ctx.settings.declare({
      id = "model",
      type = "string",
      default = "deepseek-chat",
      scope = "user",
      label = "DeepSeek Model",
      description = "Default DeepSeek model ID for chat requests.",
    })
    ctx.settings.declare({
      id = "api_key",
      type = "string",
      default = "",
      scope = "user",
      label = "DeepSeek API Key",
      description = "API key used for DeepSeek chat requests.",
    })

    local base_url = read_string(ctx, "base_url", "https://api.deepseek.com")
    local model = read_string(ctx, "model", "deepseek-chat")

    ctx.ai_providers.add({
      id = "chat",
      label = "DeepSeek",
      type = "cloud",
      models = { model },
      runtime = "openai_compat",
      base_url = base_url,
      default_model = model,
    })
  end,
})
