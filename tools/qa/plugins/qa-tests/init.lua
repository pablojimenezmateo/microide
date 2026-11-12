-- A QA-only plugin (tools/qa): a test provider for C++ and Python, so the soak's
-- `tooling` profile drives test discovery, runs and the Tests sidebar. No bundled
-- plugin provides tests, and the qa-api provider only answers for its own
-- `.qaapi` files, so `tests-discover` and `tests-run` had never run in a soak.
--
-- Discovery reads the file from disk and reports one test per function it
-- recognises, under one suite per file, with a few labels the sidebar has to lay
-- out (wide characters, a very long one). A run answers with every state the
-- host knows -- and, like a real runner, sometimes with no result for a test or a
-- result for one it was not asked about.
local ide = require("microide")

local runs = 0

local function hash(text)
  local h = 5381
  for i = 1, #text do
    h = (h * 33 + text:byte(i)) % 2147483647
  end
  return h
end

local function functions_in(text, language)
  local found = {}
  local number = 0
  for line in (text .. "\n"):gmatch("(.-)\n") do
    number = number + 1
    local name
    if language == "python" then
      name = line:match("^%s*def%s+([%w_]+)%s*%(")
    else
      -- `type name(args) {` or `name::name(args) const {`, not a call statement.
      if not line:match(";%s*$") and not line:match("^%s*//") then
        name = line:match("([%w_:~]+)%s*%b()%s*[%w%s]*{?%s*$")
      end
      if name == "if" or name == "for" or name == "while" or name == "switch" or name == "return" then
        name = nil
      end
    end
    if name then
      found[#found + 1] = { name = name, line = number }
    end
  end
  return found
end

local function discover(ctx, buffer, language)
  local path = buffer.relative_path
  if type(path) ~= "string" or path == "" then
    return {}
  end
  local text = ctx.files.read_text(path)
  if type(text) ~= "string" then
    return {}
  end
  local suite = "qa-tests/" .. path
  local tests = { { id = suite, label = path, file = path, line = 1 } }
  for _, fn in ipairs(functions_in(text, language)) do
    tests[#tests + 1] = {
      id = suite .. "::" .. fn.name .. "@" .. fn.line,
      label = fn.name,
      file = path,
      line = fn.line,
      parent_id = suite,
    }
  end
  tests[#tests + 1] = { id = suite .. "::wide", label = "名前 テスト 👨‍👩‍👧", file = path, line = 1,
                        parent_id = suite }
  tests[#tests + 1] = { id = suite .. "::long", label = string.rep("a_very_long_test_name_", 12),
                        file = path, line = 1, parent_id = suite }
  return tests
end

local STATES = { "passed", "passed", "passed", "failed", "skipped", "errored", "running" }

local function run(test_ids)
  runs = runs + 1
  local results = {}
  for _, id in ipairs(test_ids) do
    local roll = hash(id .. "#" .. runs)
    if roll % 11 ~= 0 then  -- now and then a test the runner said nothing about
      local state = STATES[roll % #STATES + 1]
      local message = state == "passed" and "ok"
          or state == "failed" and ("expected 3\nactual   4\n  at " .. id .. "\n  — ✗ mismatch")
          or state == "errored" and "the runner crashed: SIGSEGV"
          or ""
      results[#results + 1] = { test_id = id, state = state, message = message,
                                duration_ms = (roll % 5 == 0) and 0 or roll % 90000 }
    end
  end
  if runs % 5 == 0 then
    results[#results + 1] = { test_id = "qa-tests/unknown::ghost", state = "passed", message = "",
                              duration_ms = 1 }
  end
  return results
end

return ide.plugin({
  id = "qa-tests",
  capabilities = { fs = { read = "project" } },
  setup = function(ctx)
    for _, language in ipairs({ "c++", "python" }) do
      ctx.tests.add({
        id = "qa-tests." .. (language == "c++" and "cpp" or language),
        language_id = language,
        discover = function(buffer) return discover(ctx, buffer, language) end,
        run = run,
      })
    end
  end,
})
