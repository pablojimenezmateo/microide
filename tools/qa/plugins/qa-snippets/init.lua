-- A QA-only plugin (tools/qa): one C++ snippet with placeholders, so the
-- `snippets` flows have something to insert -- no bundled plugin ships snippets.
-- One snippet per engine feature: mirrors, choices, nesting, variables, and a
-- completion item whose insert text is a snippet.
local ide = require("microide")

return ide.plugin({
  id = "qa-snippets",
  setup = function(ctx)
    ctx.snippets.add({
      id = "qa-for",
      language_id = "c++",
      prefix = "fori",
      label = "for loop (QA)",
      body = "for (int ${1:i} = 0; ${1:i} < ${2:count}; ++${1:i}) {\n\t$0\n}",
    })
    local more = {
      { "mirr", "${1:name} = ${1:name} + ${2:1};$0" },
      { "pick", "mode = ${1|fast,safe,slow|};$0" },
      { "nest", "call(${1:outer(${2:inner})});$0" },
      { "vars", "// ${TM_FILENAME_BASE}:${TM_LINE_NUMBER} ${QA_UNSET:fallback}$0" },
    }
    for _, entry in ipairs(more) do
      ctx.snippets.add({
        id = "qa-" .. entry[1],
        language_id = "c++",
        prefix = entry[1],
        label = entry[1] .. " (QA)",
        body = entry[2],
      })
    end
    -- A completion item that is itself a snippet: accepting it starts a session.
    ctx.completion.add({
      id = "qa-snippets.complete",
      language_id = "c++",
      provide = function()
        return { { label = "qs_call", detail = "snippet item", insert_text = "qs_call(${1:arg}, ${2:n})$0",
                   is_snippet = true } }
      end,
    })
  end,
})
