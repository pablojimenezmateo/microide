local ide = require("microide")

-- Dummy commands used by the "demo" provider (safe default, no API key required).
local DEMO_CHAT_COMMAND = "sh -lc \"printf 'LLM example reply'\""
local DEMO_INLINE_COMMAND = "sh -lc \"printf 'llm_inline_suggestion'\""

-- Python one-liners for real API calls.
-- Use single-quotes in the shell endpoint so Python double-quote strings pass through.
-- No single quotes appear inside the Python code.

local CLAUDE_CHAT_SCRIPT = [=[import sys,json,urllib.request,os; p=sys.stdin.read(); b=json.dumps({"model":"MODELNAME","max_tokens":4096,"messages":[{"role":"user","content":p}]}).encode(); req=urllib.request.Request("https://api.anthropic.com/v1/messages",b,{"x-api-key":os.environ.get("ANTHROPIC_API_KEY",""),"anthropic-version":"2023-06-01","content-type":"application/json"}); r=urllib.request.urlopen(req); print(json.loads(r.read())["content"][0]["text"],end="")]=]

local CLAUDE_INLINE_SCRIPT = [=[import sys,json,urllib.request,os; p=sys.stdin.read(); b=json.dumps({"model":"MODELNAME","max_tokens":512,"system":"Complete the code. Output only the completion text, no explanation, no markdown fences.","messages":[{"role":"user","content":p}]}).encode(); req=urllib.request.Request("https://api.anthropic.com/v1/messages",b,{"x-api-key":os.environ.get("ANTHROPIC_API_KEY",""),"anthropic-version":"2023-06-01","content-type":"application/json"}); r=urllib.request.urlopen(req); print(json.loads(r.read())["content"][0]["text"],end="")]=]

local OPENAI_CHAT_SCRIPT = [=[import sys,json,urllib.request,os; p=sys.stdin.read(); b=json.dumps({"model":"MODELNAME","max_tokens":4096,"messages":[{"role":"user","content":p}]}).encode(); req=urllib.request.Request("https://api.openai.com/v1/chat/completions",b,{"Authorization":"Bearer "+os.environ.get("OPENAI_API_KEY",""),"content-type":"application/json"}); r=urllib.request.urlopen(req); print(json.loads(r.read())["choices"][0]["message"]["content"],end="")]=]

local OPENAI_INLINE_SCRIPT = [=[import sys,json,urllib.request,os; p=sys.stdin.read(); b=json.dumps({"model":"MODELNAME","max_tokens":512,"messages":[{"role":"system","content":"Complete the code. Output only the completion text, no explanation, no markdown fences."},{"role":"user","content":p}]}).encode(); req=urllib.request.Request("https://api.openai.com/v1/chat/completions",b,{"Authorization":"Bearer "+os.environ.get("OPENAI_API_KEY",""),"content-type":"application/json"}); r=urllib.request.urlopen(req); print(json.loads(r.read())["choices"][0]["message"]["content"],end="")]=]

local function make_endpoint(script, model)
  return "python3 -c '" .. script:gsub("MODELNAME", model) .. "'"
end

local function trim(text)
  if type(text) ~= "string" then return "" end
  return (text:gsub("^%s+", ""):gsub("%s+$", ""))
end

local function read_string(ctx, suffix, fallback)
  local v = trim(ctx.settings.get("llm." .. suffix))
  return v ~= "" and v or fallback
end

local function read_bool(ctx, suffix, fallback)
  local v = trim(ctx.settings.get("llm." .. suffix))
  if v == "" then return fallback end
  v = string.lower(v)
  return v == "1" or v == "true" or v == "yes" or v == "on"
end

local function declare_settings(ctx)
  ctx.settings.declare({
    id = "provider",
    type = "enum",
    default = "demo",
    scope = "user",
    label = "LLM Provider",
    description = "AI provider: demo (no API key needed), claude (Anthropic), or openai.",
    enum_values = { "demo", "claude", "openai" },
  })
  ctx.settings.declare({
    id = "claude.model",
    type = "string",
    default = "claude-sonnet-4-6",
    scope = "user",
    label = "Claude Model",
    description = "Anthropic model ID (e.g. claude-sonnet-4-6, claude-opus-4-7).",
  })
  ctx.settings.declare({
    id = "openai.model",
    type = "string",
    default = "gpt-4o",
    scope = "user",
    label = "OpenAI Model",
    description = "OpenAI model ID (e.g. gpt-4o, gpt-4o-mini, o1).",
  })
  ctx.settings.declare({
    id = "chat_enabled",
    type = "bool",
    default = "true",
    scope = "user",
    label = "Enable Chat Agent",
    description = "Register the LLM chat agent for the host chat command.",
  })
  ctx.settings.declare({
    id = "inline_enabled",
    type = "bool",
    default = "true",
    scope = "user",
    label = "Enable Inline Completion",
    description = "Register the LLM inline completion agent.",
  })
  ctx.settings.declare({
    id = "chat_command",
    type = "string",
    default = "",
    scope = "user",
    label = "Chat Command Override",
    description = "Custom stdio endpoint for chat. Overrides the provider when non-empty.",
  })
  ctx.settings.declare({
    id = "inline_command",
    type = "string",
    default = "",
    scope = "user",
    label = "Inline Command Override",
    description = "Custom stdio endpoint for inline completion. Overrides the provider when non-empty.",
  })
end

return ide.plugin({
  id = "llm",

  setup = function(ctx)
    declare_settings(ctx)

    local chat_enabled = read_bool(ctx, "chat_enabled", true)
    local inline_enabled = read_bool(ctx, "inline_enabled", true)
    local chat_cmd_override = read_string(ctx, "chat_command", "")
    local inline_cmd_override = read_string(ctx, "inline_command", "")

    -- Build provider-default commands and register provider metadata.
    local default_chat_cmd, default_inline_cmd, provider_label
    local provider = read_string(ctx, "provider", "demo")

    if provider == "openai" then
      local model = read_string(ctx, "openai.model", "gpt-4o")
      default_chat_cmd = make_endpoint(OPENAI_CHAT_SCRIPT, model)
      default_inline_cmd = make_endpoint(OPENAI_INLINE_SCRIPT, model)
      provider_label = "OpenAI"
      ctx.ai_providers.add({
        id = "openai",
        label = "OpenAI",
        type = "cloud",
        models = { model, "gpt-4o", "gpt-4o-mini", "o1", "o3-mini" },
      })
    elseif provider == "claude" then
      local model = read_string(ctx, "claude.model", "claude-sonnet-4-6")
      default_chat_cmd = make_endpoint(CLAUDE_CHAT_SCRIPT, model)
      default_inline_cmd = make_endpoint(CLAUDE_INLINE_SCRIPT, model)
      provider_label = "Anthropic Claude"
      ctx.ai_providers.add({
        id = "claude",
        label = "Anthropic Claude",
        type = "cloud",
        models = { model, "claude-sonnet-4-6", "claude-opus-4-7", "claude-haiku-4-5-20251001" },
      })
    else
      -- "demo" or unrecognised: dummy commands, no API key required.
      default_chat_cmd = DEMO_CHAT_COMMAND
      default_inline_cmd = DEMO_INLINE_COMMAND
      provider_label = "Demo LLM"
    end

    -- Per-command overrides: non-empty setting wins over provider default.
    local chat_cmd = chat_cmd_override ~= "" and chat_cmd_override or default_chat_cmd
    local inline_cmd = inline_cmd_override ~= "" and inline_cmd_override or default_inline_cmd

    if chat_enabled and chat_cmd ~= nil and chat_cmd ~= "" then
      ctx.external_agents.add({
        id = "chat",
        label = provider_label .. " Chat",
        protocol = "stdio",
        endpoint = chat_cmd,
        capabilities = { "chat" },
      })
    end

    if inline_enabled and inline_cmd ~= nil and inline_cmd ~= "" then
      ctx.external_agents.add({
        id = "inline",
        label = provider_label .. " Inline",
        protocol = "stdio",
        endpoint = inline_cmd,
        capabilities = { "inline-completion" },
      })
    end
  end,
})
