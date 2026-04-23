local ide = require("microide")

-- Auth helper embedded in endpoint scripts.
-- All string literals use double quotes; single quotes must not appear here
-- because the endpoint string is wrapped in single quotes by the shell.
local AUTH_HELPER = [=[
import json,os,time,base64,urllib.request,urllib.parse,datetime
_AF=os.path.expanduser("~/.codex/auth.json")
_CID="app_EMoamEEZ73f0CkXaXp7hrann"
_TU="https://auth.openai.com/oauth/token"
def _exp(t):
    try:
        p=t.split(".")[1];p+=("="*(-len(p)%4))
        return json.loads(base64.urlsafe_b64decode(p)).get("exp",0)
    except:
        return 0
def _refresh(auth,toks,rt):
    body=urllib.parse.urlencode({"grant_type":"refresh_token","client_id":_CID,"refresh_token":rt}).encode()
    req=urllib.request.Request(_TU,body,{"content-type":"application/x-www-form-urlencoded"})
    resp=json.loads(urllib.request.urlopen(req,timeout=10).read())
    toks["access_token"]=resp["access_token"]
    if "refresh_token" in resp:toks["refresh_token"]=resp["refresh_token"]
    auth["tokens"]=toks
    auth["last_refresh"]=datetime.datetime.utcnow().isoformat()+"Z"
    with open(_AF,"w") as f:json.dump(auth,f,indent=2)
    return resp["access_token"]
def get_token():
    env=os.environ.get("OPENAI_API_KEY","")
    if not os.path.exists(_AF):
        if env:return env
        raise SystemExit("Not authenticated. Run llm-login.")
    with open(_AF) as f:auth=json.load(f)
    toks=auth.get("tokens",{})
    at=toks.get("access_token","");rt=toks.get("refresh_token","")
    if not at:
        if env:return env
        raise SystemExit("No token. Run llm-login.")
    if _exp(at)>time.time()+60:return at
    if rt:
        try:return _refresh(auth,toks,rt)
        except:pass
    return at
]=]

-- Chat endpoint: stdin → one user turn → stdout. Must be single-quote-free.
local CHAT_SCRIPT_TMPL = AUTH_HELPER .. [=[
import sys
try:
    p=sys.stdin.read();t=get_token()
    body=json.dumps({"model":"MODELID","max_tokens":4096,"messages":[{"role":"user","content":p}]}).encode()
    req=urllib.request.Request("https://api.openai.com/v1/chat/completions",body,{"Authorization":"Bearer "+t,"content-type":"application/json"})
    print(json.loads(urllib.request.urlopen(req,timeout=60).read())["choices"][0]["message"]["content"],end="")
except SystemExit as e:
    print(str(e),file=sys.stderr);sys.exit(1)
except Exception as e:
    print("API error: "+str(e),file=sys.stderr);sys.exit(1)
]=]

-- Inline endpoint: same structure, code-completion system prompt. Must be single-quote-free.
local INLINE_SCRIPT_TMPL = AUTH_HELPER .. [=[
import sys
try:
    p=sys.stdin.read();t=get_token()
    body=json.dumps({"model":"MODELID","max_tokens":512,"messages":[{"role":"system","content":"Complete the code. Output only the completion text, no explanation, no markdown fences."},{"role":"user","content":p}]}).encode()
    req=urllib.request.Request("https://api.openai.com/v1/chat/completions",body,{"Authorization":"Bearer "+t,"content-type":"application/json"})
    print(json.loads(urllib.request.urlopen(req,timeout=30).read())["choices"][0]["message"]["content"],end="")
except SystemExit as e:
    print(str(e),file=sys.stderr);sys.exit(1)
except Exception as e:
    print("API error: "+str(e),file=sys.stderr);sys.exit(1)
]=]

-- Scripts below run via ctx.process.run_async argv arrays, so any characters are allowed.

local DEVICE_CODE_SCRIPT = [=[
import json,urllib.request,urllib.parse,sys
CLIENT_ID="app_EMoamEEZ73f0CkXaXp7hrann"
DEVICE_URL="https://auth.openai.com/oauth/device/code"
SCOPE="openid profile email offline_access"
body=urllib.parse.urlencode({"client_id":CLIENT_ID,"scope":SCOPE}).encode()
req=urllib.request.Request(DEVICE_URL,body,{"content-type":"application/x-www-form-urlencoded"})
try:
    resp=json.loads(urllib.request.urlopen(req,timeout=10).read())
    print("device_code="+resp["device_code"])
    print("user_code="+resp.get("user_code",""))
    print("verification_uri="+resp.get("verification_uri",resp.get("verification_url","")))
    print("expires_in="+str(resp.get("expires_in",300)))
    print("interval="+str(resp.get("interval",5)))
except Exception as e:
    print("error="+str(e),file=sys.stderr)
    sys.exit(1)
]=]

-- Polls for token completion. device_code is passed as sys.argv[1].
local POLL_SCRIPT = [=[
import json,urllib.request,urllib.parse,os,datetime,sys,base64
CLIENT_ID="app_EMoamEEZ73f0CkXaXp7hrann"
TOKEN_URL="https://auth.openai.com/oauth/token"
AUTH_FILE=os.path.expanduser("~/.codex/auth.json")
device_code=sys.argv[1]
body=urllib.parse.urlencode({"grant_type":"urn:ietf:params:oauth:grant-type:device_code","device_code":device_code,"client_id":CLIENT_ID}).encode()
req=urllib.request.Request(TOKEN_URL,body,{"content-type":"application/x-www-form-urlencoded"})
try:
    resp=json.loads(urllib.request.urlopen(req,timeout=10).read())
    if "access_token" in resp:
        account=""
        if "id_token" in resp:
            try:
                p=resp["id_token"].split(".")[1];p+="="*(-len(p)%4)
                claims=json.loads(base64.urlsafe_b64decode(p))
                account=claims.get("email",claims.get("sub",""))
            except:pass
        auth={"auth_mode":"chatgpt","tokens":{"access_token":resp["access_token"],"refresh_token":resp.get("refresh_token",""),"id_token":resp.get("id_token","")},"last_refresh":datetime.datetime.utcnow().isoformat()+"Z"}
        os.makedirs(os.path.dirname(AUTH_FILE),exist_ok=True)
        with open(AUTH_FILE,"w") as f:json.dump(auth,f,indent=2)
        print("status=authorized")
        if account:print("account="+account)
    else:
        err=resp.get("error","")
        if err in ("authorization_pending","slow_down"):print("status=pending")
        else:print("status=error\nerror="+resp.get("error_description",err))
except Exception as e:
    print("status=error\nerror="+str(e))
]=]

local STATUS_SCRIPT = [=[
import json,os,time,base64
AUTH_FILE=os.path.expanduser("~/.codex/auth.json")
def _exp(t):
    try:
        p=t.split(".")[1];p+="="*(-len(p)%4)
        return json.loads(base64.urlsafe_b64decode(p)).get("exp",0)
    except:return 0
env=os.environ.get("OPENAI_API_KEY","")
if os.path.exists(AUTH_FILE):
    with open(AUTH_FILE) as f:auth=json.load(f)
    toks=auth.get("tokens",{})
    at=toks.get("access_token","")
    if at:
        exp=_exp(at);now=time.time()
        if exp>now:
            print("status=authenticated")
            print("expires_in="+str(int(exp-now)))
            try:
                p=at.split(".")[1];p+="="*(-len(p)%4)
                claims=json.loads(base64.urlsafe_b64decode(p))
                sub=claims.get("email",claims.get("sub",""))
                if sub:print("account="+sub)
            except:pass
        else:
            rt=toks.get("refresh_token","")
            print("status=expired_refreshable" if rt else "status=expired")
    elif env:print("status=api_key")
    else:print("status=unauthenticated")
elif env:print("status=api_key")
else:print("status=unauthenticated")
]=]

local LOGOUT_SCRIPT = [=[
import json,os
AUTH_FILE=os.path.expanduser("~/.codex/auth.json")
if os.path.exists(AUTH_FILE):
    with open(AUTH_FILE) as f:auth=json.load(f)
    auth["tokens"]={}
    with open(AUTH_FILE,"w") as f:json.dump(auth,f,indent=2)
    print("ok")
else:
    print("not_found")
]=]

local function trim(text)
  if type(text) ~= "string" then return "" end
  return (text:gsub("^%s+", ""):gsub("%s+$", ""))
end

local function parse_kv(text)
  local t = {}
  for line in ((text or "") .. "\n"):gmatch("([^\n]+)\n") do
    local k, v = line:match("^([^=]+)=(.*)$")
    if k then t[trim(k)] = trim(v) end
  end
  return t
end

local function read_string(ctx, key, fallback)
  local v = trim(ctx.settings.get("llm." .. key))
  return v ~= "" and v or fallback
end

local function read_bool(ctx, key, fallback)
  local v = trim(ctx.settings.get("llm." .. key))
  if v == "" then return fallback end
  v = string.lower(v)
  return v == "1" or v == "true" or v == "yes" or v == "on"
end

local function make_endpoint(script, model)
  return "python3 -c '" .. script:gsub("MODELID", model) .. "'"
end

local function resolve_agent_endpoint(ctx, key, default_endpoint)
  local override = read_string(ctx, key, "")
  if override ~= "" then
    return override
  end
  return default_endpoint
end

return ide.plugin({
  id = "llm",

  setup = function(ctx)
    ctx.settings.declare({
      id = "codex.model",
      type = "string",
      default = "gpt-5.4",
      scope = "user",
      label = "Codex Model",
      description = "OpenAI model ID for Codex-backed chat and inline completion.",
    })
    ctx.settings.declare({
      id = "chat_enabled",
      type = "bool",
      default = "true",
      scope = "user",
      label = "Enable Chat Agent",
      description = "Register the Codex chat agent.",
    })
    ctx.settings.declare({
      id = "inline_enabled",
      type = "bool",
      default = "true",
      scope = "user",
      label = "Enable Inline Completion",
      description = "Register the Codex inline completion agent.",
    })
    ctx.settings.declare({
      id = "chat_command",
      type = "string",
      default = "",
      scope = "user",
      label = "Chat Command Override",
      description = "Optional stdio command used instead of the built-in Codex chat endpoint.",
    })
    ctx.settings.declare({
      id = "inline_command",
      type = "string",
      default = "",
      scope = "user",
      label = "Inline Command Override",
      description = "Optional stdio command used instead of the built-in Codex inline endpoint.",
    })

    -- llm-login: OAuth 2.0 device authorization grant (RFC 8628) against auth.openai.com.
    -- Reuses ~/.codex/auth.json so credentials are shared with the Codex CLI.
    ctx.commands.add("llm-login", function(cmd_ctx, args)
      ctx.process.run_async({"python3", "-c", DEVICE_CODE_SCRIPT}, {}, function(res1)
        if res1.exit_code ~= 0 then
          ctx.log("[codex] Login failed: " .. trim(res1.stderr))
          return
        end
        local kv1 = parse_kv(res1.stdout)
        local device_code = kv1.device_code
        local uri = kv1.verification_uri
        local user_code = kv1.user_code
        local interval = math.max(tonumber(kv1.interval) or 5, 5)
        local expires_in = tonumber(kv1.expires_in) or 300
        local max_polls = math.floor(expires_in / interval)

        ctx.log("[codex] Open this URL in your browser to authenticate:")
        ctx.log("[codex]   " .. (uri ~= "" and uri or "(no URL returned — check output channel)"))
        if user_code and user_code ~= "" then
          ctx.log("[codex] Enter code: " .. user_code)
        end
        ctx.log("[codex] Waiting for authorization (timeout in " .. expires_in .. "s)...")

        local function poll(attempts)
          if attempts <= 0 then
            ctx.log("[codex] Login timed out. Please run llm-login again.")
            return
          end
          ctx.process.run_async({"sleep", tostring(interval)}, {}, function(_)
            ctx.process.run_async({"python3", "-c", POLL_SCRIPT, device_code}, {}, function(res2)
              local kv2 = parse_kv(res2.stdout)
              local status = kv2.status or "error"
              if status == "authorized" then
                local msg = "[codex] Authenticated successfully."
                if kv2.account and kv2.account ~= "" then
                  msg = "[codex] Authenticated as " .. kv2.account .. "."
                end
                ctx.log(msg)
              elseif status == "pending" then
                poll(attempts - 1)
              else
                ctx.log("[codex] Login error: " .. (kv2.error or "unknown"))
              end
            end)
          end)
        end

        poll(max_polls)
      end)
    end)

    ctx.commands.add("llm-logout", function(cmd_ctx, args)
      ctx.process.run_async({"python3", "-c", LOGOUT_SCRIPT}, {}, function(res)
        if trim(res.stdout) == "ok" then
          ctx.log("[codex] Logged out. Run llm-login to re-authenticate.")
        else
          ctx.log("[codex] Not logged in (no auth file found).")
        end
      end)
    end)

    ctx.commands.add("llm-status", function(cmd_ctx, args)
      ctx.process.run_async({"python3", "-c", STATUS_SCRIPT}, {}, function(res)
        local kv = parse_kv(res.stdout)
        local status = kv.status or "unknown"
        if status == "authenticated" then
          local msg = "[codex] Authenticated"
          if kv.account and kv.account ~= "" then
            msg = msg .. " as " .. kv.account
          end
          if kv.expires_in then
            local mins = math.floor((tonumber(kv.expires_in) or 0) / 60)
            msg = msg .. " (token valid for ~" .. mins .. "m)"
          end
          ctx.log(msg .. ".")
        elseif status == "expired_refreshable" then
          ctx.log("[codex] Token expired; will auto-refresh on next API call.")
        elseif status == "expired" then
          ctx.log("[codex] Token expired. Run: llm-login")
        elseif status == "api_key" then
          ctx.log("[codex] Using OPENAI_API_KEY environment variable.")
        else
          ctx.log("[codex] Not authenticated. Run: llm-login")
        end
      end)
    end)

    -- External agents
    local model = read_string(ctx, "codex.model", "gpt-5.4")
    local chat_enabled = read_bool(ctx, "chat_enabled", true)
    local inline_enabled = read_bool(ctx, "inline_enabled", true)
    local chat_endpoint =
      resolve_agent_endpoint(ctx, "chat_command", make_endpoint(CHAT_SCRIPT_TMPL, model))
    local inline_endpoint =
      resolve_agent_endpoint(ctx, "inline_command", make_endpoint(INLINE_SCRIPT_TMPL, model))

    if chat_enabled then
      ctx.external_agents.add({
        id = "chat",
        label = "Codex Chat",
        protocol = "stdio",
        endpoint = chat_endpoint,
        capabilities = {"chat"},
      })
    end

    if inline_enabled then
      ctx.external_agents.add({
        id = "inline",
        label = "Codex Inline",
        protocol = "stdio",
        endpoint = inline_endpoint,
        capabilities = {"inline-completion"},
      })
    end
  end,
})
