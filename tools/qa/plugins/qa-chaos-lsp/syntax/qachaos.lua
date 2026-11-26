return {
  filetype = "qachaos",
  files = { "\\.qachaos$" },
  rules = {
    { pattern = "\\bdef\\b", group = "keyword" },
  },
}
