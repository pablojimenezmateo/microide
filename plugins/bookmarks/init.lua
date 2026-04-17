local ide = require("microide")

local SIDEBAR_ID = "project-bookmarks"
local STORAGE_PATH = ".microide/bookmarks.tsv"

local function trim(text)
  return (text:gsub("^%s+", ""):gsub("%s+$", ""))
end

local function sanitize_label(label)
  if type(label) ~= "string" then
    return nil
  end
  label = trim(label:gsub("[\r\n\t]", " "))
  if label == "" then
    return nil
  end
  return label
end

local function default_label(buffer)
  return buffer.relative_path .. ":" .. tostring(buffer.line) .. ":" .. tostring(buffer.column)
end

local function load_entries(ctx)
  local text = ctx.files.read_text(STORAGE_PATH)
  if type(text) ~= "string" or text == "" then
    return {}
  end

  local entries = {}
  for line in text:gmatch("([^\n]+)") do
    local path, row, column, label = line:match("^([^\t]+)\t([^\t]+)\t([^\t]+)\t(.*)$")
    if path ~= nil and tonumber(row) ~= nil and tonumber(column) ~= nil then
      entries[#entries + 1] = {
        path = path,
        line = tonumber(row),
        column = tonumber(column),
        label = label ~= "" and label or (path .. ":" .. row .. ":" .. column),
      }
    end
  end
  return entries
end

local function save_entries(ctx, entries)
  local lines = {}
  for _, entry in ipairs(entries) do
    lines[#lines + 1] = table.concat({
      entry.path,
      tostring(entry.line),
      tostring(entry.column),
      entry.label,
    }, "\t")
  end

  local content = table.concat(lines, "\n")
  if #lines > 0 then
    content = content .. "\n"
  end
  return ctx.files.write_text(STORAGE_PATH, content)
end

local function snapshot_items(ctx)
  local items = {}
  for _, entry in ipairs(load_entries(ctx)) do
    items[#items + 1] = {
      label = entry.label,
      detail = entry.path .. ":" .. tostring(entry.line) .. ":" .. tostring(entry.column),
      path = entry.path,
      line = entry.line,
      column = entry.column,
    }
  end
  return items
end

return ide.plugin({
  id = "bookmarks",

  setup = function(ctx)
    ctx.sidebar.add({
      id = SIDEBAR_ID,
      label = "Bookmarks",
      snapshot = function()
        return snapshot_items(ctx)
      end,
      on_confirm = function(item)
        ctx.workspace.open_file(item.path, item.line, item.column)
      end,
    })

    ctx.commands.add("bookmarks.add", function(ctx, args)
      local buffer = ctx.workspace.active_buffer()
      if buffer == nil or type(buffer.relative_path) ~= "string" or buffer.relative_path == "" then
        ctx.log("bookmarks: no active editable buffer")
        return
      end

      local label = sanitize_label(table.concat(args, " ")) or default_label(buffer)
      local entries = load_entries(ctx)
      entries[#entries + 1] = {
        path = buffer.relative_path,
        line = buffer.line,
        column = buffer.column,
        label = label,
      }

      if save_entries(ctx, entries) then
        ctx.sidebar.show(SIDEBAR_ID)
        ctx.log("bookmarks: added " .. label)
      else
        ctx.log("bookmarks: failed to save bookmarks")
      end
    end)

    ctx.commands.add("bookmarks.show", function(ctx, args)
      ctx.sidebar.show(SIDEBAR_ID)
    end)

    ctx.commands.add("bookmarks.clear", function(ctx, args)
      if ctx.files.write_text(STORAGE_PATH, "") then
        ctx.log("bookmarks: cleared")
      else
        ctx.log("bookmarks: failed to clear bookmarks")
      end
    end)
  end,
})
