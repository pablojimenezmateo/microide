local ide = require("microide")

-- Dogfood example for the Phase C plugin-rendering surfaces:
--
--   * A TREE sidebar: `snapshot` returns a flattened tree (each row tagged with
--     `depth` / `collapsible` / `collapsed`); the host draws indentation and a
--     disclosure twisty. The plugin owns expand/collapse state and reshapes its
--     snapshot when the host calls `on_toggle(item)`.
--   * The four plugin-native LANGUAGE providers: go-to-definition, find
--     references, signature help, and document symbols. The host owns the
--     surfaces (navigation, the References output channel, the outline) and only
--     asks this plugin for data.
--
-- It is intentionally data-only and self-contained (no network, no subprocess),
-- so it can be copied into ~/.config/microide/plugins to exercise the surfaces.

local LANGUAGE_ID = "lua"

-- A tiny in-memory outline the tree sidebar renders. `expanded` is plugin-owned
-- state toggled through `on_toggle`.
local groups = {
  { id = "g.types", label = "Types", expanded = true,
    children = {
      { label = "Widget", detail = "class", path = "src/widget.lua", line = 4 },
      { label = "Button", detail = "class", path = "src/widget.lua", line = 40 },
    } },
  { id = "g.funcs", label = "Functions", expanded = false,
    children = {
      { label = "main", detail = "function", path = "src/main.lua", line = 1 },
    } },
}

local function find_group(id)
  for _, group in ipairs(groups) do
    if group.id == id then
      return group
    end
  end
  return nil
end

return ide.plugin({
  id = "language-tools",
  setup = function(ctx)
    -- ---- Tree sidebar ----------------------------------------------------
    ctx.sidebar.add({
      id = "language-tools",
      label = "Symbols",
      snapshot = function()
        local rows = {}
        for _, group in ipairs(groups) do
          rows[#rows + 1] = {
            id = group.id,
            label = group.label,
            depth = 0,
            collapsible = true,
            collapsed = not group.expanded,
          }
          if group.expanded then
            for _, child in ipairs(group.children) do
              rows[#rows + 1] = {
                label = child.label,
                detail = child.detail,
                depth = 1,
                path = child.path,
                line = child.line,
              }
            end
          end
        end
        return rows
      end,
      on_toggle = function(item)
        local group = find_group(item.id)
        if group then
          group.expanded = not group.expanded
        end
      end,
      on_confirm = function(item)
        if item.path then
          ctx.workspace.open_file(item.path, item.line or 1, item.column or 1)
        end
      end,
    })

    -- ---- Language providers ---------------------------------------------
    ctx.definition.add({
      id = "defn",
      language_id = LANGUAGE_ID,
      provide = function(buffer, position)
        return {
          { path = buffer.path, line = math.max(1, position.line - 1), column = 1 },
        }
      end,
    })

    ctx.references.add({
      id = "refs",
      language_id = LANGUAGE_ID,
      provide = function(buffer, position, include_declaration)
        local results = {
          { path = buffer.path, line = position.line, column = position.column },
        }
        if include_declaration then
          results[#results + 1] = { path = buffer.path, line = 1, column = 1 }
        end
        return results
      end,
    })

    ctx.signature_help.add({
      id = "sig",
      language_id = LANGUAGE_ID,
      provide = function(_, _)
        return {
          active_signature = 0,
          signatures = {
            {
              label = "greet(name: string)",
              documentation = "Greets the given name.",
              active_parameter = 0,
              parameters = { { label = "name" } },
            },
          },
        }
      end,
    })

    ctx.document_symbols.add({
      id = "symbols",
      language_id = LANGUAGE_ID,
      provide = function(_)
        return {
          {
            name = "Widget", kind = "class", line = 4, column = 1,
            children = {
              { name = "draw", kind = "method", line = 8, column = 3 },
            },
          },
          { name = "main", kind = "function", line = 40, column = 1 },
        }
      end,
    })
  end,
})
