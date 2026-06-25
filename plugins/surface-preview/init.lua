local ide = require("microide")

-- Dogfood example for the Phase E plugin content surfaces. It is data-only (no
-- network, no subprocess), so it can be copied into ~/.config/microide/plugins to
-- exercise the surfaces:
--
--   * DISPLAY LIST: `ctx.surface.set(id, spec)` ships a flat op buffer (rects /
--     lines / text / polyline / clip) the host replays with its own renderer.
--     Plugins never touch SDL. `preview = "bottom"` opens it as a bottom-panel
--     tab; `hit_regions` make parts clickable (a click runs a host command).
--   * RASTER (documented below): a real chart/markdown/mermaid plugin would run a
--     CLI via `ctx.process.run_async` (capability-gated, off-thread) and hand the
--     resulting PNG bytes to `ctx.surface.set(id, { raster = { format="png",
--     bytes=<...> } })`. The HOST never spawns the tool and never fetches a URL;
--     it only decodes the plugin's local bytes. Omitted here to stay self-contained.
--   * INLINE INSET (experimental): `anchor = { path, line }` renders the surface as
--     an inert block inset below that line when `plugins.inline_surfaces` is on.

-- A small bar chart as a display list. Coordinates are content-local (0,0 = the
-- surface's top-left); the host translates + clips them to the panel/inset.
local function bar_chart(values, palette)
  local ops = {}
  local width, height = 320, 160
  local base_y = height - 24
  local bar_w = 36
  ops[#ops + 1] = { op = "rect", x = 0, y = 0, w = width, h = height, color = "#1b1f2a" }
  ops[#ops + 1] = { op = "line", x1 = 8, y1 = base_y, x2 = width - 8, y2 = base_y, color = "#3a4154" }
  for i, v in ipairs(values) do
    local x = 16 + (i - 1) * (bar_w + 16)
    local h = math.floor((v / 100) * (base_y - 16))
    ops[#ops + 1] = { op = "rect", x = x, y = base_y - h, w = bar_w, h = h,
                      color = palette[((i - 1) % #palette) + 1] }
    ops[#ops + 1] = { op = "text", x = x, y = base_y + 4, text = tostring(v), color = "#aab2c5" }
  end
  ops[#ops + 1] = { op = "text", x = 12, y = 8, text = "Sample metrics", color = "#e6e9f0" }
  return { width = width, height = height, ops = ops }, width, height
end

return ide.plugin({
  id = "surface-preview",
  setup = function(ctx)
    local palette = { "#5aa9e6", "#7fc8a9", "#f6c177", "#eb6f92" }

    local function publish(values)
      local dl = bar_chart(values, palette)
      ctx.surface.set("metrics-chart", {
        title = "Metrics",
        preview = "bottom",
        display_list = dl,
        hit_regions = {
          -- The title bar re-rolls the chart when clicked.
          { x = 0, y = 0, w = 320, h = 20, command = "surface-preview.refresh" },
        },
      })
    end

    -- Show / refresh the bottom-panel preview.
    ctx.commands.add("surface-preview.show", function()
      publish({ 40, 75, 30, 90 })
    end)
    ctx.commands.add("surface-preview.refresh", function()
      publish({ math.random(20, 100), math.random(20, 100), math.random(20, 100), math.random(20, 100) })
    end)
    ctx.commands.add("surface-preview.hide", function()
      ctx.surface.clear("metrics-chart")
    end)

    -- Inline inset anchored to the active buffer's first line (only visible when
    -- the experimental `plugins.inline_surfaces` setting is on).
    ctx.commands.add("surface-preview.inline", function()
      local buffer = ctx.workspace.active_buffer()
      if buffer == nil or buffer.path == nil then
        return
      end
      local dl = bar_chart({ 60, 35, 80 }, palette)
      ctx.surface.set("inline-chart", {
        title = "Inline",
        anchor = { path = buffer.path, line = 1 },
        display_list = dl,
      })
    end)
  end,
})
