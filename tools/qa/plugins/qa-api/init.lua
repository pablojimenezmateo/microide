-- QA-only: one of each plugin provider and event, for tools/qa's `plugin-api` flow.
-- Every callback the host makes is counted, the counts written to qa-api-seen.txt in
-- the project (a status item that long would not fit the bar, which drops it), so
-- the flow checks each path ran through the real UI (the coverage build of the suite
-- ran almost none of them: TD-2026-10-18-364).
local ide = require("microide")

local LANG = "qaapi"
local seen = {}
local order = { "change", "cursor", "selection", "close", "ping", "hover", "complete", "action",
                "definition", "references", "signature", "symbols", "confirm", "toggle",
                "discover", "run" }
local expanded = true

local function bump(ctx, what)
  seen[what] = (seen[what] or 0) + 1
  local parts = {}
  for _, key in ipairs(order) do
    if seen[key] then parts[#parts + 1] = key .. "=" .. seen[key] end
  end
  ctx.files.write_text("qa-api-seen.txt", table.concat(parts, " ") .. "\n")
  local total = 0
  for _, count in pairs(seen) do total = total + count end
  ctx.status.update("seen", { text = "qa-api " .. tostring(total) })
end

return ide.plugin({
  id = "qa-api",
  capabilities = { process = { exec = true } },

  setup = function(ctx)
    ctx.status.add({ id = "seen", text = "qa-api", tooltip = "QA API callbacks", alignment = "left", priority = 1 })
    ctx.commands.add("qa.api.ping", function(cmd_ctx, args)
      bump(ctx, "ping")
      return "pong:" .. table.concat(args or {}, ",")
    end)
    -- Problems for the `problems-tests-views` flow, without a language server:
    -- two in notes.qaapi, one in other.qaapi; `qa.api.problems.clear` drops them.
    ctx.commands.add("qa.api.problems", function(cmd_ctx, args)
      ctx.diagnostics.publish("notes.qaapi", {
        { message = "qa error on two", severity = "error", line = 2, column = 1 },
        { message = "qa warning on three", severity = "warning", line = 3, column = 2 },
      })
      ctx.diagnostics.publish("other.qaapi", {
        { message = "qa info in other", severity = "info", line = 1, column = 1 },
      })
      return "published"
    end)
    ctx.commands.add("qa.api.problems.clear", function(cmd_ctx, args)
      ctx.diagnostics.clear(nil)
      return "cleared"
    end)
    -- Host-owned buffer edits and the ghost-text seam, for `plugin-editor-api`:
    -- each returns what the host answered, so the flow reads the refusal too.
    local function answer(ok, err) return ok and "ok" or ("refused:" .. tostring(err)) end
    ctx.commands.add("qa.api.edit", function(cmd_ctx, args)
      return answer(ctx.editor.apply_edits({
        edits = {
          { start_line = 1, start_col = 1, end_line = 1, end_col = 3, text = "EDIT" },
          { start_line = 2, start_col = 1, end_line = 2, end_col = 1, text = "-- " },
        },
        cursor = { line = 2, col = 4 },
      }))
    end)
    ctx.commands.add("qa.api.edit.closed", function(cmd_ctx, args)
      return answer(ctx.editor.apply_edits({
        path = "never-opened.qaapi",
        edits = { { start_line = 1, start_col = 1, end_line = 1, end_col = 1, text = "x" } },
      }))
    end)
    ctx.commands.add("qa.api.cursor", function(cmd_ctx, args)
      return answer(ctx.editor.set_cursor({ line = 3, col = 2 }))
    end)
    ctx.commands.add("qa.api.select", function(cmd_ctx, args)
      return answer(ctx.editor.set_selection({ start_line = 1, start_col = 1, end_line = 1, end_col = 5 }))
    end)
    ctx.commands.add("qa.api.ghost", function(cmd_ctx, args)
      return answer(ctx.editor.set_ghost_text({ text = "GHOST" }))
    end)
    ctx.commands.add("qa.api.ghost.clear", function(cmd_ctx, args)
      ctx.editor.clear_ghost_text()
      return "cleared"
    end)
    ctx.commands.add("qa.api.notify", function(cmd_ctx, args)
      ctx.log("qa-api: notify asked")
      ctx.notify("warning", "qa-api says hello")
      return "notified"
    end)
    ctx.menus.add({ id = "ping-menu", menu = "Help", action = "qa.api.ping", label = "QA API Ping" })
    ctx.keybindings.add({ id = "ping-key", action = "qa.api.ping", key = "Ctrl+Alt+U", context = "editor" })

    ctx.hover.add({
      id = "qa-api.hover",
      provide = function(buffer, position)
        if buffer.path and buffer.path:match("%.qaapi$") and position.line == 1 then
          bump(ctx, "hover")
          return { title = "QA API", content = "qa hover line " .. tostring(position.line) }
        end
        return nil
      end,
    })
    ctx.completion.add({
      id = "qa-api.complete", language_id = LANG, trigger_characters = { "." },
      provide = function(buffer, position, trigger)
        bump(ctx, "complete")
        return {
          { label = "qa_alpha", detail = "first", documentation = "the first", insert_text = "qa_alpha" },
          { label = "qa_beta", detail = "second", insert_text = "qa_beta" },
        }
      end,
    })
    ctx.code_actions.add({
      id = "qa-api.action", language_id = LANG,
      provide = function(buffer, range)
        bump(ctx, "action")
        return { { title = "QA API: ping", command = "qa.api.ping", arguments = { "from-action" } } }
      end,
    })
    ctx.definition.add({
      id = "qa-api.def", language_id = LANG,
      provide = function(buffer, position)
        bump(ctx, "definition")
        return { { path = buffer.path, line = 1, column = 4 } }
      end,
    })
    ctx.references.add({
      id = "qa-api.ref", language_id = LANG,
      provide = function(buffer, position, include_declaration)
        bump(ctx, "references")
        return { { path = buffer.path, line = 1, column = 4 }, { path = buffer.path, line = 3, column = 5 } }
      end,
    })
    ctx.signature_help.add({
      id = "qa-api.sig", language_id = LANG,
      provide = function()
        bump(ctx, "signature")
        return {
          active_signature = 0,
          signatures = { { label = "greet(name, times)", documentation = "says hello", active_parameter = 0,
                           parameters = { { label = "name" }, { label = "times" } } } },
        }
      end,
    })
    ctx.document_symbols.add({
      id = "qa-api.sym", language_id = LANG,
      provide = function()
        bump(ctx, "symbols")
        return { { name = "greet", kind = "function", line = 1, column = 1,
                   children = { { name = "inner", kind = "variable", line = 2, column = 3 } } } }
      end,
    })
    ctx.sidebar.add({
      id = "qa-api", label = "QA API",
      snapshot = function()
        local rows = { { id = "root", label = "Root", depth = 0, collapsible = true, collapsed = not expanded } }
        if expanded then
          rows[#rows + 1] = { id = "leaf", label = "Leaf", depth = 1, path = "notes.qaapi", line = 3 }
        end
        return rows
      end,
      on_toggle = function(item)
        bump(ctx, "toggle")
        if item.id == "root" then expanded = not expanded end
      end,
      on_confirm = function(item)
        bump(ctx, "confirm")
      end,
    })
    ctx.tests.add({
      id = "qa-api.tests", language_id = LANG,
      discover = function(buffer)
        bump(ctx, "discover")
        return { { id = "qa.case", label = "QA case", file = buffer.relative_path, line = 1 } }
      end,
      run = function(test_ids)
        bump(ctx, "run")
        return { { test_id = test_ids[1], state = "passed", message = "ok", duration_ms = 1 } }
      end,
    })
  end,

  on_buffer_change = function(ctx, buffer, change) bump(ctx, "change") end,
  on_cursor_move = function(ctx, buffer, pos) bump(ctx, "cursor") end,
  on_selection_change = function(ctx, buffer, sel) if sel then bump(ctx, "selection") end end,
  on_buffer_close = function(ctx, buffer) bump(ctx, "close") end,
})
