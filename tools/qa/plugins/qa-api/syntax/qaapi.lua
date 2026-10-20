return {
  filetype = "qaapi",
  files = { "\\.qaapi$" },
  rules = {
    { pattern = "\\b(fn|let)\\b", group = "keyword" },
  },
}
