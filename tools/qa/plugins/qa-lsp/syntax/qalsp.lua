return {
  filetype = "qalsp",
  files = { "\\.qalsp$" },
  rules = {
    { pattern = "\\buse\\b", group = "keyword" },
  },
}
