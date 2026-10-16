-- A QA-only plugin (tools/qa): one C++ snippet with placeholders, so the
-- `snippets` flow has something to insert -- no bundled plugin ships snippets.
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
  end,
})
