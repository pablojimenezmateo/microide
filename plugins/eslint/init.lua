local ide = require("microide")
local json = require("json")

local SUPPORTED_EXTENSIONS = {
  [".js"] = true,
  [".jsx"] = true,
  [".mjs"] = true,
  [".cjs"] = true,
  [".ts"] = true,
  [".tsx"] = true,
}

local session_files = {}

local function file_extension(path)
  local extension = path:match("^.+(%.[^./\\]+)$")
  if extension == nil then
    return nil
  end
  return string.lower(extension)
end

local function is_supported_path(path)
  local extension = file_extension(path)
  return extension ~= nil and SUPPORTED_EXTENSIONS[extension] == true
end

local function eslint_binary(ctx)
  if ctx.files.exists("node_modules/.bin/eslint") then
    return "node_modules/.bin/eslint"
  end
  return "eslint"
end

local function active_relative_path(ctx)
  local buffer = ctx.workspace.active_buffer()
  if buffer == nil then
    return nil
  end
  return buffer.relative_path
end

local function diagnostic_severity(message)
  if tonumber(message.severity) == 2 then
    return "error"
  end
  return "warning"
end

local function diagnostic_message(message)
  local text = type(message.message) == "string" and message.message or "ESLint issue"
  if type(message.ruleId) == "string" and message.ruleId ~= "" then
    return text .. " (" .. message.ruleId .. ")"
  end
  return text
end

local function build_diagnostics(report)
  local diagnostics = {}
  if type(report) ~= "table" or type(report.messages) ~= "table" then
    return diagnostics
  end

  for _, message in ipairs(report.messages) do
    if type(message) == "table" and type(message.line) == "number" and type(message.column) == "number" then
      local line = math.max(1, math.floor(message.line))
      local column = math.max(1, math.floor(message.column))
      local end_line = type(message.endLine) == "number" and math.max(line, math.floor(message.endLine)) or line
      local end_column = type(message.endColumn) == "number" and math.max(1, math.floor(message.endColumn)) or (column + 1)
      if end_line == line and end_column < column then
        end_column = column + 1
      end

      diagnostics[#diagnostics + 1] = {
        message = diagnostic_message(message),
        severity = diagnostic_severity(message),
        line = line,
        column = column,
        end_line = end_line,
        end_column = end_column,
      }
    end
  end

  return diagnostics
end

local function ensure_session_entry(ctx, relative_path)
  if type(relative_path) ~= "string" or relative_path == "" or not is_supported_path(relative_path) then
    return nil
  end

  local entry = session_files[relative_path]
  if entry ~= nil then
    return entry
  end

  entry = {
    baseline_text = ctx.files.read_text(relative_path) or "",
    dirty = false,
    issue_count = 0,
  }
  session_files[relative_path] = entry
  return entry
end

local function refresh_session_entry(ctx, relative_path)
  local entry = ensure_session_entry(ctx, relative_path)
  if entry == nil then
    return nil
  end

  local text = ctx.files.read_text(relative_path)
  if type(text) ~= "string" then
    session_files[relative_path] = nil
    return nil
  end

  entry.dirty = text ~= entry.baseline_text
  return entry
end

local function lint_path(ctx, relative_path, quiet, force)
  if type(relative_path) ~= "string" or relative_path == "" then
    if not quiet then
      ctx.log("eslint: no target file")
    end
    return false
  end
  if not is_supported_path(relative_path) then
    if not quiet then
      ctx.log("eslint: unsupported file type for " .. relative_path)
    end
    return false
  end

  local entry = force and ensure_session_entry(ctx, relative_path) or refresh_session_entry(ctx, relative_path)
  if not force and (entry == nil or not entry.dirty) then
    ctx.diagnostics.clear(relative_path)
    if entry ~= nil then
      entry.issue_count = 0
    end
    if not quiet then
      ctx.log("eslint: " .. relative_path .. " is clean for this session")
    end
    return true
  end

  local result = ctx.process.run({
    eslint_binary(ctx),
    "--no-error-on-unmatched-pattern",
    "--format",
    "json",
    relative_path,
  }, {
    cwd = ".",
  })

  if result.exit_code ~= 0 and result.exit_code ~= 1 then
    local failure_text = result.stderr ~= "" and result.stderr or result.stdout
    ctx.log("eslint: failed for " .. relative_path .. ": " .. failure_text)
    return false
  end

  local ok, reports = pcall(json.decode, result.stdout)
  if not ok or type(reports) ~= "table" then
    ctx.log("eslint: invalid JSON output for " .. relative_path)
    return false
  end

  local diagnostics = build_diagnostics(reports[1] or {})
  if #diagnostics == 0 then
    ctx.diagnostics.clear(relative_path)
  else
    ctx.diagnostics.publish(relative_path, diagnostics)
  end

  if entry ~= nil then
    entry.issue_count = #diagnostics
  end
  if not quiet then
    ctx.log("eslint: " .. relative_path .. ": " .. tostring(#diagnostics) .. " issue(s)")
  end
  return true
end

local function dirty_opened_paths(ctx)
  local paths = {}
  for relative_path, entry in pairs(session_files) do
    if is_supported_path(relative_path) then
      local refreshed = refresh_session_entry(ctx, relative_path)
      if refreshed ~= nil and refreshed.dirty then
        paths[#paths + 1] = relative_path
      end
    end
  end
  table.sort(paths)
  return paths
end

return ide.plugin({
  id = "eslint",

  setup = function(ctx)
    ctx.commands.add("eslint.run", function(ctx, args)
      local relative_path = args[1] or active_relative_path(ctx)
      lint_path(ctx, relative_path, false, true)
    end)

    ctx.commands.add("eslint.run-opened", function(ctx, args)
      local paths = dirty_opened_paths(ctx)
      if #paths == 0 then
        ctx.log("eslint: no dirty opened files")
        return
      end

      local ok_count = 0
      for _, relative_path in ipairs(paths) do
        if lint_path(ctx, relative_path, true, false) then
          ok_count = ok_count + 1
        end
      end
      ctx.log("eslint: linted " .. tostring(ok_count) .. " dirty opened file(s)")
    end)

    ctx.commands.add("eslint.clear", function(ctx, args)
      local relative_path = args[1] or active_relative_path(ctx)
      if type(relative_path) ~= "string" or relative_path == "" then
        ctx.log("eslint: no target file")
        return
      end
      ctx.diagnostics.clear(relative_path)
      local entry = session_files[relative_path]
      if entry ~= nil then
        entry.issue_count = 0
      end
      ctx.log("eslint: cleared diagnostics for " .. relative_path)
    end)

    ctx.commands.add("eslint.show-problems", function(ctx, args)
      ctx.sidebar.show("problems")
    end)
  end,

  on_buffer_open = function(ctx, buffer)
    if buffer ~= nil and type(buffer.relative_path) == "string" then
      ensure_session_entry(ctx, buffer.relative_path)
    end
  end,

  on_buffer_save = function(ctx, buffer)
    if buffer ~= nil and type(buffer.relative_path) == "string" then
      lint_path(ctx, buffer.relative_path, true, false)
    end
  end,
})
