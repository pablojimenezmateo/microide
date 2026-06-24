local ide = require("microide")

-- Dogfood example for the `ctx.decorations` host capability: highlights TODO /
-- FIXME / XXX / HACK markers in comments with a colored text style and a gutter
-- bookmark, recomputed whenever a buffer opens or is saved. The host owns all
-- rendering; this plugin only publishes decoration data.

local MARKERS = {
  TODO = "#e0a96d",
  FIXME = "#e06c75",
  XXX = "#e06c75",
  HACK = "#c678dd",
}

-- Scan one line's bytes for any marker keyword, returning the 1-based [start,end]
-- columns of the first hit and its color (or nil when the line has no marker).
local function find_marker(line)
  local best_start, best_end, best_color
  for keyword, color in pairs(MARKERS) do
    local s, e = string.find(line, keyword, 1, true)
    if s and (best_start == nil or s < best_start) then
      best_start, best_end, best_color = s, e, color
    end
  end
  return best_start, best_end, best_color
end

local function refresh(ctx, buffer)
  local path = buffer.relative_path
  if type(path) ~= "string" or path == "" then
    return
  end
  local text = ctx.files.read_text(path)
  if type(text) ~= "string" then
    ctx.decorations.clear(path)
    return
  end

  local text_styles = {}
  local gutter_marks = {}
  local line_number = 1
  for line in (text .. "\n"):gmatch("(.-)\n") do
    local s, e, color = find_marker(line)
    if s then
      text_styles[#text_styles + 1] = {
        line = line_number,
        start_col = s,
        end_col = e + 1,   -- end_col is exclusive
        fg = color,
        bold = true,
      }
      gutter_marks[#gutter_marks + 1] = {
        line = line_number,
        icon = "bookmark",
        color = color,
        priority = 1,
      }
    end
    line_number = line_number + 1
  end

  ctx.decorations.set(path, { text_styles = text_styles, gutter_marks = gutter_marks })
end

return ide.plugin({
  id = "todo-highlight",

  -- Only needs to read project files; no process or network access.
  capabilities = {
    fs = { read = "project" },
  },

  on_buffer_open = function(ctx, buffer)
    refresh(ctx, buffer)
  end,

  on_buffer_save = function(ctx, buffer)
    refresh(ctx, buffer)
  end,
})
