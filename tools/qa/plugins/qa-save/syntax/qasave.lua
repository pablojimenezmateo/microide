return {
  filetype = "qasave",
  files = { "\\.qasave$" },
  rules = {
    { pattern = "\\bqa\\b", group = "keyword" },
  },
}
