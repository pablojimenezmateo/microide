# docs/media — assets for the showcase page

Drop the showcase media here. `../index.html` references these filenames; add the
files and they light up automatically (no markup changes needed for the names below).

## Demo video (combined hero)

One take that walks through multiple projects, diff & merge, the debugger, and
LLM control.

| File | Purpose | Required? |
|------|---------|-----------|
| `hero-demo.mp4`   | H.264/AAC MP4 — the broadly-compatible source | Yes |
| `hero-demo.webm`  | VP9/Opus WebM — smaller, optional second source | Optional |
| `hero-poster.png` | Still frame shown before the video plays | Optional but recommended |

Tips:
- Keep the file reasonably small — it ships in the git repo and loads on GitHub Pages.
  Consider `-crf` ~28 and a capped resolution (e.g. 1080p) when encoding.
- `preload="none"` is set, so the video only downloads once the visitor hits play.

## Screenshots

The gallery in `index.html` references placeholder names. Either drop in PNGs with
these names and swap the placeholder `<div class="frame">` for an `<img>`, or rename
to taste:

- `shot-1.png`, `shot-2.png`, `shot-3.png`

Suggested shots: editor with git blame, a three-way merge, the debugger paused on a
breakpoint with the variables pane open.

## Open Graph image (optional)

- `og.png` — 1200×630 social preview. Uncomment the `og:image`/`og:url` tags in
  `index.html` once it exists and the site URL is final.
