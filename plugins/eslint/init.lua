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
local next_request_generation = 0

local function file_extension(path)
  local extension = path:match("^.+(%.[^./\\]+)$")
  if extension == nil then
    return nil
  end
  return string.lower(extension)
end

local function is_eslint_path(path)
  local extension = file_extension(path)
  return extension ~= nil and SUPPORTED_EXTENSIONS[extension] == true
end

local function is_typescript_config_path(path)
  if type(path) ~= "string" or path == "" then
    return false
  end
  local basename = path:match("([^/\\]+)$")
  if basename == nil then
    return false
  end
  basename = string.lower(basename)
  return basename:match("^tsconfig.*%.json$") ~= nil or basename:match("^jsconfig.*%.json$") ~= nil
end

local function supported_path_kind(path)
  if is_eslint_path(path) then
    return "eslint"
  end
  if is_typescript_config_path(path) then
    return "typescript-config"
  end
  return nil
end

local function is_windows_project_root(project_root)
  return type(project_root) == "string" and project_root:match("^%a:[/\\]") ~= nil
end

local function append_args(prefix, suffix)
  local command = {}
  for _, value in ipairs(prefix) do
    command[#command + 1] = value
  end
  for _, value in ipairs(suffix) do
    command[#command + 1] = value
  end
  return command
end

local function has_yarn_lock(ctx)
  return ctx.files.exists("yarn.lock")
end

local function sanitize_path_fragment(path)
  return tostring(path or ""):gsub("[/\\]", "__"):gsub("[^%w%._-]", "_")
end

local function eslint_report_path(ctx, relative_path)
  -- Write the report into the plugin's sandboxed data dir rather than /tmp: the capability
  -- sandbox confines plugin file access (and spawned-process writes) to the project root and
  -- this data directory, so /tmp is no longer readable/writable from the plugin.
  local data_dir = ctx.workspace.data_dir()
  if type(data_dir) ~= "string" or data_dir == "" then
    data_dir = "."
  end
  local project_root = ctx.workspace.project_root()
  local project_safe = sanitize_path_fragment(project_root ~= "" and project_root or "project")
  local relative_safe = sanitize_path_fragment(relative_path)
  return data_dir .. "/eslint-" .. project_safe .. "-" .. relative_safe .. ".json"
end

local function first_line(text)
  text = type(text) == "string" and text or ""
  return text:match("([^\r\n]+)") or text
end

local function shell_quote(value)
  value = tostring(value or "")
  return "'" .. value:gsub("'", [['"'"']]) .. "'"
end

local function shell_command(argv)
  local parts = {}
  for _, value in ipairs(argv) do
    parts[#parts + 1] = shell_quote(value)
  end
  return table.concat(parts, " ")
end

local function shell_with_user_env(argv)
  return shell_command(argv)
end

local function local_binary_command(ctx, name, args)
  local project_root = ctx.workspace.project_root()
  if type(project_root) ~= "string" or project_root == "" then
    return nil
  end

  local base_relative_path = "node_modules/.bin/" .. name
  if is_windows_project_root(project_root) then
    local cmd_relative_path = base_relative_path .. ".cmd"
    if ctx.files.exists(cmd_relative_path) then
      return append_args({ "cmd.exe", "/d", "/c", project_root .. "/" .. cmd_relative_path }, args)
    end
    if ctx.files.exists(base_relative_path) then
      return {
        "bash",
        "-lc",
        shell_with_user_env(append_args({ project_root .. "/" .. base_relative_path }, args)),
      }
    end
  end

  if ctx.files.exists(base_relative_path) then
    return append_args({ project_root .. "/" .. base_relative_path }, args)
  end

  return nil
end

local function eslint_command(ctx, relative_path, report_path, use_yarn)
  if use_yarn then
    return {
      "bash",
      "-lc",
      shell_with_user_env({
        "yarn",
        "-s",
        "eslint",
        "--no-error-on-unmatched-pattern",
        "--format",
        "json",
        "--output-file",
        report_path,
        relative_path,
      }),
    }
  end
  local args = {
    "--no-error-on-unmatched-pattern",
    "--format",
    "json",
    "--output-file",
    report_path,
    relative_path,
  }
  return local_binary_command(ctx, "eslint", args) or append_args({ "eslint" }, args)
end

local function tsc_command(ctx, relative_path)
  if has_yarn_lock(ctx) then
    return {
        "bash",
        "-lc",
        shell_with_user_env({
        "tsc",
        "-p",
        relative_path,
        "--pretty",
        "false",
      }),
    }
  end
  local args = {
    "-p",
    relative_path,
    "--pretty",
    "false",
  }
  return local_binary_command(ctx, "tsc", args) or append_args({ "tsc" }, args)
end

local function eslint_requires_yarn(result)
  local failure_text = result.stderr ~= "" and result.stderr or result.stdout
  failure_text = type(failure_text) == "string" and failure_text or ""
  return failure_text:find("ERR_REQUIRE_ESM", 1, true) ~= nil
end

local function decode_eslint_reports(output)
  output = type(output) == "string" and output or ""
  local ok, reports = pcall(json.decode, output)
  if ok and type(reports) == "table" then
    return reports
  end

  local start_index = output:find("%[", 1)
  local end_index = output:match(".*()%]")
  if start_index == nil or end_index == nil or end_index < start_index then
    return nil
  end

  ok, reports = pcall(json.decode, output:sub(start_index, end_index))
  if ok and type(reports) == "table" then
    return reports
  end
  return nil
end

local function decode_eslint_reports_from_result(result)
  local stdout_text = type(result.stdout) == "string" and result.stdout or ""
  local stderr_text = type(result.stderr) == "string" and result.stderr or ""

  local reports = decode_eslint_reports(stdout_text)
  if type(reports) == "table" then
    return reports
  end

  reports = decode_eslint_reports(stderr_text)
  if type(reports) == "table" then
    return reports
  end

  reports = decode_eslint_reports(stdout_text .. "\n" .. stderr_text)
  if type(reports) == "table" then
    return reports
  end

  return nil
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

local function typescript_config_severity(level)
  level = type(level) == "string" and string.lower(level) or ""
  if level == "error" then
    return "error"
  end
  if level == "info" or level == "information" then
    return "info"
  end
  if level == "hint" then
    return "hint"
  end
  return "warning"
end

local function normalize_path(path)
  return type(path) == "string" and path:gsub("\\", "/") or ""
end

local function build_typescript_config_diagnostics(relative_path, output)
  local diagnostics = {}
  local normalized_target = normalize_path(relative_path)
  for line in tostring(output or ""):gmatch("[^\r\n]+") do
    local path_text, line_text, column_text, level, message =
        line:match("^(.-)%((%d+),(%d+)%)%: ([^:]+)%: (.+)$")
    if path_text ~= nil and normalize_path(path_text) == normalized_target then
      local line_number = tonumber(line_text)
      local column_number = tonumber(column_text)
      if line_number ~= nil and column_number ~= nil then
        diagnostics[#diagnostics + 1] = {
          message = message,
          severity = typescript_config_severity(level),
          line = math.max(1, math.floor(line_number)),
          column = math.max(1, math.floor(column_number)),
          end_line = math.max(1, math.floor(line_number)),
          end_column = math.max(1, math.floor(column_number + 1)),
        }
      end
    end
  end
  return diagnostics
end

local function ensure_session_entry(ctx, relative_path)
  local kind = supported_path_kind(relative_path)
  if type(relative_path) ~= "string" or relative_path == "" or kind == nil then
    return nil
  end

  local entry = session_files[relative_path]
  if entry ~= nil then
    return entry
  end

  entry = {
    kind = kind,
    baseline_text = ctx.files.read_text(relative_path) or "",
    dirty = false,
    issue_count = 0,
    request_generation = 0,
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

local function finish_eslint_result(ctx, relative_path, quiet, entry, result, report_path)
  if result.exit_code ~= 0 and result.exit_code ~= 1 then
    local failure_text = result.stderr ~= "" and result.stderr or result.stdout
    ctx.log("eslint: failed for " .. relative_path .. ": " .. failure_text)
    return false
  end

  local report_text = ctx.files.read_text(report_path)
  local reports = type(report_text) == "string" and decode_eslint_reports(report_text) or nil
  if type(reports) ~= "table" then
    reports = decode_eslint_reports_from_result(result)
  end
  if type(reports) ~= "table" then
    local report_summary = type(report_text) == "string" and first_line(report_text) or "<missing>"
    local stderr_summary = first_line(result.stderr)
    local stdout_summary = first_line(result.stdout)
    ctx.log(
      "eslint: invalid JSON output for "
        .. relative_path
        .. " (report="
        .. report_summary
        .. ", stderr="
        .. stderr_summary
        .. ", stdout="
        .. stdout_summary
        .. ")"
    )
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

local function run_eslint(ctx, relative_path, quiet, entry, force)
  if not force and (entry == nil or not entry.dirty) then
    if not quiet then
      ctx.log("eslint: " .. relative_path .. " is unchanged for this session")
    end
    return true
  end

  local report_path = eslint_report_path(ctx, relative_path)
  ctx.files.write_text(report_path, "")
  local use_yarn = has_yarn_lock(ctx)
  local result = ctx.process.run(eslint_command(ctx, relative_path, report_path, use_yarn), {
    cwd = ".",
  })

  if (result.exit_code ~= 0 and result.exit_code ~= 1) and use_yarn == false and eslint_requires_yarn(result) then
    ctx.files.write_text(report_path, "")
    result = ctx.process.run(eslint_command(ctx, relative_path, report_path, true), {
      cwd = ".",
    })
  end

  return finish_eslint_result(ctx, relative_path, quiet, entry, result, report_path)
end

local function finish_typescript_config_result(ctx, relative_path, quiet, entry, result)
  local output = result.stdout ~= "" and result.stdout or result.stderr
  local diagnostics = build_typescript_config_diagnostics(relative_path, output)
  if result.exit_code ~= 0 and #diagnostics == 0 then
    ctx.log("eslint: failed to check TypeScript config " .. relative_path .. ": " .. output)
    return false
  end

  if #diagnostics == 0 then
    ctx.diagnostics.clear(relative_path)
  else
    ctx.diagnostics.publish(relative_path, diagnostics)
  end

  if entry ~= nil then
    entry.issue_count = #diagnostics
  end
  if not quiet then
    ctx.log("eslint: " .. relative_path .. ": " .. tostring(#diagnostics) .. " TypeScript config issue(s)")
  end
  return true
end

local function run_typescript_config_check(ctx, relative_path, quiet, entry)
  local result = ctx.process.run({
    table.unpack(tsc_command(ctx, relative_path)),
  }, {
    cwd = ".",
  })
  return finish_typescript_config_result(ctx, relative_path, quiet, entry, result)
end

local function begin_entry_request(entry)
  next_request_generation = next_request_generation + 1
  entry.request_generation = next_request_generation
  return entry.request_generation
end

local function request_is_current(entry, generation)
  return entry ~= nil and entry.request_generation == generation
end

local function lint_path_async(ctx, relative_path, quiet, force)
  if type(relative_path) ~= "string" or relative_path == "" then
    return false
  end
  local kind = supported_path_kind(relative_path)
  if kind == nil then
    return false
  end

  local entry = force and ensure_session_entry(ctx, relative_path) or refresh_session_entry(ctx, relative_path)
  if entry == nil then
    return false
  end

  if entry.kind == "typescript-config" then
    if not force and not entry.dirty then
      return true
    end

    local generation = begin_entry_request(entry)
    ctx.process.run_async({
      table.unpack(tsc_command(ctx, relative_path)),
    }, {
      cwd = ".",
    }, function(result)
      if not request_is_current(entry, generation) then
        return
      end
      finish_typescript_config_result(ctx, relative_path, quiet, entry, result)
    end)
    return true
  end

  if not force and not entry.dirty then
    return true
  end

  local report_path = eslint_report_path(ctx, relative_path)
  local generation = begin_entry_request(entry)
  local use_yarn = has_yarn_lock(ctx)
  ctx.files.write_text(report_path, "")
  ctx.process.run_async(eslint_command(ctx, relative_path, report_path, use_yarn), {
    cwd = ".",
  }, function(result)
    if not request_is_current(entry, generation) then
      return
    end

    if (result.exit_code ~= 0 and result.exit_code ~= 1) and use_yarn == false and eslint_requires_yarn(result) then
      ctx.files.write_text(report_path, "")
      ctx.process.run_async(eslint_command(ctx, relative_path, report_path, true), {
        cwd = ".",
      }, function(fallback_result)
        if not request_is_current(entry, generation) then
          return
        end
        finish_eslint_result(ctx, relative_path, quiet, entry, fallback_result, report_path)
      end)
      return
    end

    finish_eslint_result(ctx, relative_path, quiet, entry, result, report_path)
  end)
  return true
end

local function lint_path(ctx, relative_path, quiet, force)
  if type(relative_path) ~= "string" or relative_path == "" then
    if not quiet then
      ctx.log("eslint: no target file")
    end
    return false
  end
  local kind = supported_path_kind(relative_path)
  if kind == nil then
    if not quiet then
      ctx.log("eslint: unsupported file type for " .. relative_path)
    end
    return false
  end

  local entry = force and ensure_session_entry(ctx, relative_path) or refresh_session_entry(ctx, relative_path)
  if entry == nil then
    return false
  end

  if entry.kind == "typescript-config" then
    if not force and not entry.dirty then
      return true
    end
    return run_typescript_config_check(ctx, relative_path, quiet, entry)
  end
  return run_eslint(ctx, relative_path, quiet, entry, force)
end

local function opened_paths()
  local paths = {}
  for relative_path, entry in pairs(session_files) do
    if supported_path_kind(relative_path) ~= nil then
      paths[#paths + 1] = relative_path
    end
  end
  table.sort(paths)
  return paths
end

return ide.plugin({
  id = "eslint",

  -- Reads project sources and writes lint reports into the plugin data dir ("data" scope grants
  -- both the project tree and the data dir), and spawns eslint/tsc/yarn/npx, so it needs process
  -- execution. No allowlist: the binary varies (project-local node_modules/.bin, bash, yarn, npx).
  capabilities = {
    fs = { read = "data", write = "data" },
    process = { exec = true },
  },

  setup = function(ctx)
    ctx.commands.add("eslint.run", function(ctx, args)
      local relative_path = args[1] or active_relative_path(ctx)
      lint_path(ctx, relative_path, false, true)
    end)

    ctx.commands.add("eslint.run-opened", function(ctx, args)
      local paths = opened_paths()
      if #paths == 0 then
        ctx.log("eslint: no opened files")
        return
      end

      for _, relative_path in ipairs(paths) do
        lint_path(ctx, relative_path, true, true)
      end
      ctx.log("eslint: linting " .. tostring(#paths) .. " opened file(s)")
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
      lint_path_async(ctx, buffer.relative_path, true, true)
    end
  end,

  on_buffer_save = function(ctx, buffer)
    if buffer ~= nil and type(buffer.relative_path) == "string" then
      lint_path_async(ctx, buffer.relative_path, true, true)
    end
  end,
})
