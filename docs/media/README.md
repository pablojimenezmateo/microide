# docs/media — showcase assets (generated)

Everything in this directory is **generated**, not hand-recorded. `../index.html`
references these filenames; regenerate them with one command:

```bash
tools/capture-media.sh            # all assets (screenshots + hero video)
tools/capture-media.sh --shots-only
tools/capture-media.sh --video-only
```

The pipeline runs a headless microide on a private Xvfb display, drives it through
the control channel, and captures the frames — it never touches the real desktop
or your real microide config. Full details + the scene list:
[`dev-docs/project/media-generation.md`](../../dev-docs/project/media-generation.md).

## Files

| File | What | Produced by |
|------|------|-------------|
| `shot-editor.png`    | Editor: highlighting, tree, blame, terminal | `capture-shots.sh` |
| `shot-control.png`   | LLM/script driving the control channel       | `capture-shots.sh` |
| `shot-git-diff.png`  | Side-by-side working-tree diff               | `capture-shots.sh` |
| `shot-git-merge.png` | Three-way merge conflict view                | `capture-shots.sh` |
| `shot-dap.png`       | Debugger paused on a breakpoint              | `capture-shots.sh` |
| `hero-demo.mp4`      | Labeled hero trailer — H.264/MP4 (primary source) | `record-hero.sh` |
| `hero-demo.webm`     | Labeled hero trailer — VP9/WebM (smaller source)  | `record-hero.sh` |
| `hero-poster.png`    | Mid-action still shown before the video plays     | `record-hero.sh` |
| `og.png`             | 1200×630 social preview (og:image / twitter:card) — hero-poster contained on `#0d1117` | by hand |

## Regenerate on any UI change

**If a change touches the UI, these assets are stale and must be regenerated**
(`tools/capture-media.sh`) and re-committed. This is a release-checklist gate — see
[`dev-docs/project/release-checklist.md`](../../dev-docs/project/release-checklist.md).
`tools/release.sh` regenerates them automatically as part of a release.

## Dependencies

`Xvfb`, `ffmpeg` (libx264 + libvpx-vp9), ImageMagick (`import`/`convert`),
`xdotool`, and — for the debugger scene — `gdb` plus the bundled `gdb-dap` plugin.
On Debian/Ubuntu: `sudo apt install xvfb ffmpeg imagemagick xdotool gdb`.
