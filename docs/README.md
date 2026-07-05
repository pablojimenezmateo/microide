# docs — GitHub Pages source

This directory is published as the project site via GitHub Pages
(Settings → Pages → Source: `main` branch, `/docs` folder). `.nojekyll` disables
Jekyll so files are served as-is.

- `index.html` — the single-page showcase site (self-contained: inline CSS, no JS,
  no external requests).
- `media/` — demo video, screenshots, and social image. See `media/README.md`.

## Preview locally

Open `index.html` directly in a browser, or serve the folder to mimic the Pages
path layout:

```bash
python3 -m http.server -d docs 8000
# then open http://localhost:8000
```
