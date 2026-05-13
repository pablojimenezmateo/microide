These `.microide` files were originally vendored from the legacy `micro/runtime/colorschemes/`
tree before the repo was flattened.

`microide` loads them at runtime through `src/render/Theme.cpp` and maps the
old Micro highlight groups onto the SDL shell theme.

`default.microide` mirrors the built-in default returned for the special
`default` colorscheme name. `microide-classic-dark.microide` preserves the
previous built-in dark palette for users who prefer that look.
