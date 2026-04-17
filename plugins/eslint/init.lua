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

local function lint_path(ctx, relative_path, quiet)
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

  if not quiet then
    ctx.log("eslint: " .. relative_path .. ": " .. tostring(#diagnostics) .. " issue(s)")
  end
  return true
end

return ide.plugin({
  id = "eslint",

  setup = function(ctx)
    ctx.commands.add("eslint.run", function(ctx, args)
      local relative_path = args[1] or active_relative_path(ctx)
      lint_path(ctx, relative_path, false)
    end)

    ctx.commands.add("eslint.clear", function(ctx, args)
      local relative_path = args[1] or active_relative_path(ctx)
      if type(relative_path) ~= "string" or relative_path == "" then
        ctx.log("eslint: no target file")
        return
      end
      ctx.diagnostics.clear(relative_path)
      ctx.log("eslint: cleared diagnostics for " .. relative_path)
    end)

    ctx.commands.add("eslint.show-problems", function(ctx, args)
      ctx.sidebar.show("problems")
    end)
  end,

  on_buffer_save = function(ctx, buffer)
    if buffer ~= nil and type(buffer.relative_path) == "string" then
      lint_path(ctx, buffer.relative_path, true)
    end
  end,
})
