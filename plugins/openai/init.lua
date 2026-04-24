local ide = require("microide")

local function trim(text)
  return (text or ""):match("^%s*(.-)%s*$")
end

local function read_string(ctx, key, default_value)
  local value = trim(ctx.settings.get("openai." .. key))
  if value == "" then
    return default_value
  end
  return value
end

return ide.plugin({
  id = "openai",

  setup = function(ctx)
    ctx.settings.declare({
      id = "binary",
      type = "string",
      default = "microide_provider_bridge",
      scope = "user",
      label = "OpenAI Bridge Binary",
      description = "Path to the native OpenAI provider bridge binary.",
    })
    ctx.settings.declare({
      id = "base_url",
      type = "string",
      default = "https://api.openai.com",
      scope = "user",
      label = "OpenAI Base URL",
      description = "Base URL for the OpenAI API.",
    })
    ctx.settings.declare({
      id = "model",
      type = "string",
      default = "gpt-4.1-mini",
      scope = "user",
      label = "OpenAI Model",
      description = "Default OpenAI model ID for chat requests.",
    })

    local binary = read_string(ctx, "binary", "microide_provider_bridge")
    local base_url = read_string(ctx, "base_url", "https://api.openai.com")
    local model = read_string(ctx, "model", "gpt-4.1-mini")

    ctx.ai_providers.add({
      id = "chat",
      label = "OpenAI",
      type = "cloud",
      models = { model },
    })

    ctx.external_agents.add({
      id = "chat",
      label = "OpenAI",
      protocol = "stdio",
      command = {
        binary,
        "--provider", "openai",
        "--base-url", base_url,
        "--default-model", model,
      },
      capabilities = { "chat" },
    })
  end,
})
