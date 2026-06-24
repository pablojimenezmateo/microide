local ide = require("microide")

-- Dogfood example for the Phase B `ctx.decorations` rendering paths: end-of-line
-- inline virtual text and clickable code lenses. The host owns all drawing; this
-- plugin only publishes decoration data and a command for the lens to invoke.
--
--   * Inline EOL text (Error-Lens style): every line containing a `BUG:` marker
--     gets a dimmed message painted past the line's last glyph.
--   * Code lens: line 1 of every buffer gets a clickable "microide ✦ greet (N)"
--     affordance bound to a command; clicking it runs the command, which bumps a
--     per-file counter and re-publishes so the lens label updates in place.

local MARKER = "BUG:"
local LENS_COMMAND = "eol-annotations.greet"

-- Per-file click counter, keyed by relative path. Demonstrates that a code-lens
-- click round-trips through the host command dispatch back into the plugin.
local clicks_by_path = {}

local function publish(ctx, path)
  if type(path) ~= "string" or path == "" then
    return
  end
  local text = ctx.files.read_text(path)
  if type(text) ~= "string" then
    ctx.decorations.clear(path)
    return
  end

  local inline_text = {}
  local line_number = 1
  for line in (text .. "\n"):gmatch("(.-)\n") do
    local marker = string.find(line, MARKER, 1, true)
    if marker then
      -- The message after the marker, rendered dimly at end of line.
      local message = line:sub(marker + #MARKER):gsub("^%s+", "")
      if message == "" then
        message = "tracked bug"
      end
      inline_text[#inline_text + 1] = {
        line = line_number,
        text = "⚠ " .. message,
        color = "#e0a96d",
        eol = true,
      }
    end
    line_number = line_number + 1
  end

  local clicks = clicks_by_path[path] or 0
  local code_lenses = {
    {
      line = 1,
      text = string.format("microide ✦ greet (%d)", clicks),
      command = LENS_COMMAND,
    },
  }

  ctx.decorations.set(path, { inline_text = inline_text, code_lenses = code_lenses })
end

return ide.plugin({
  id = "eol-annotations",

  -- Only needs to read project files; no process or network access.
  capabilities = {
    fs = { read = "project" },
  },

  setup = function(ctx)
    -- The code lens dispatches this command on click. It bumps the active file's
    -- counter and re-publishes so the lens label reflects the new value.
    ctx.commands.add(LENS_COMMAND, function(ctx, _args)
      local buffer = ctx.workspace.active_buffer()
      if buffer == nil or type(buffer.relative_path) ~= "string" then
        return
      end
      local path = buffer.relative_path
      clicks_by_path[path] = (clicks_by_path[path] or 0) + 1
      ctx.log("eol-annotations: greeted from " .. path)
      publish(ctx, path)
    end)
  end,

  on_buffer_open = function(ctx, buffer)
    if buffer ~= nil then
      publish(ctx, buffer.relative_path)
    end
  end,

  on_buffer_save = function(ctx, buffer)
    if buffer ~= nil then
      publish(ctx, buffer.relative_path)
    end
  end,
})
