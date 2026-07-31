# Showcase media generation

The GitHub Pages site (`docs/index.html`) ships a hero video and a five-shot
feature gallery. **All of it is generated** by a scripted, reproducible pipeline —
never hand-recorded — so the media tracks the real UI and can be rebuilt on demand.

> **Standing rule:** if a change touches the UI, the committed media in
> `docs/media/` is stale. Regenerate it (`tools/capture-media.sh`) and re-commit.
> `tools/release.sh` does this automatically; the release checklist enforces it.

## One command

```bash
tools/capture-media.sh              # screenshots + hero video → docs/media/
tools/capture-media.sh --shots-only
tools/capture-media.sh --video-only
tools/capture-media.sh --hidpi      # 2× framebuffer, downsampled (crisper, slower)
tools/capture-media.sh --keep-work  # keep the scratch tree for debugging
```

It builds `build/microide/microide` if missing, then runs the two capture
scripts. Outputs: `shot-{editor,control,git-diff,git-merge,dap}.png`,
`hero-demo.{mp4,webm}`, and `hero-poster.png`.

## How it works

Files live in `tools/capture-media/`:

- **`lib.sh`** — the shared recipe. Starts a private **Xvfb** virtual display,
  forces the **X11 SDL driver** (`SDL_VIDEODRIVER=x11`) so the window is
  capturable, isolates *all* on-disk state in a throwaway XDG tree (the real
  microide config/session is never touched), seeds the bundled plugins (notably
  `gdb-dap`), drives the instance via the **control channel**, injects keystrokes
  with **xdotool**, and grabs frames through `cm_capture`. Because it runs
  entirely on Xvfb it never steals the real desktop and works headless / in CI.
  The grabber is ImageMagick `import` when present and otherwise `xwd` piped
  through `xwd2png.py` (x11-apps + Pillow), so stills and the repro scripts run
  on a box with no ImageMagick — only the hero *video* still needs ffmpeg.
- **`make-fixture.sh`** — builds the deterministic demo project (`taskflow`): a
  small C++ git repo with pinned author/dates, a compiled `-g` binary for the
  debugger, and a divergent branch engineered to conflict for the merge scene.
- **`capture-shots.sh`** — one fresh instance per scene → the five PNGs.
- **`record-hero.sh`** — one instance, seven short ffmpeg `x11grab` clips, and a
  stitched hero trailer (editor → search → terminal → diff → merge → debugger →
  control channel). The post-process step preserves the full frame, adds
  bottom-right labels, concatenates the beats, and encodes MP4 + WebM + poster.

### Why these tools

The app has **no built-in screenshot/export**. The control channel
(`dev-docs/control/control-channel.md`) is the seam used to set up each state
deterministically (`open`, `review-branch`, `review-conflicts`, `breakpoint-set`,
`debug-run --wait stopped`, `colorscheme`, …); xdotool supplies the live typing
and the few keystrokes with no control verb (e.g. `Ctrl+W`); ffmpeg + ImageMagick
do the encoding. Capture geometry defaults to the app's native 1440×900. The
hero trailer uses only ffmpeg filters that ship with the normal dependency set
(`scale`, `drawtext`, `concat`) and records with the X11 mouse cursor hidden.

## Scene map

| Asset | Feature | Key control-channel / keystroke drive |
|-------|---------|---------------------------------------|
| `shot-editor.png`    | Editor             | `open`, `sidebar-show tree`, dark `colorscheme` |
| `shot-control.png`   | Control channel    | live `microide control-send` transcript in the terminal + a channel-set breakpoint |
| `shot-git-diff.png`  | Working-tree diff  | `review-branch` after a scripted edit |
| `shot-git-merge.png` | Three-way merge    | `git merge` (conflict) + `review-conflicts` |
| `shot-dap.png`       | Debugger paused    | `breakpoint-set` + `debug-run --type gdb --wait stopped` + `debug-pane-variables` |

## Hero trailer

`record-hero.sh` keeps the public output contract unchanged
(`hero-demo.{mp4,webm}` + `hero-poster.png`) but generates the video as a
roughly 36 second full-frame trailer rather than a passive unlabeled recording.

| Beat | Target length | What it shows |
|------|---------------|---------------|
| Editor | 4.8s | Larger generated source file with visible scrolling |
| Search | 4.0s | Project search results across the fixture workspace |
| Terminal | 4.4s | PTY-backed terminal running the fixture binary |
| Diff | 5.4s | Rich scheduler rewrite shown in the working-tree diff |
| Merge | 5.6s | Real `git merge` conflict opened in the merge surface |
| Debugger | 6.6s | DAP breakpoint hit, variables visible, step commands |
| Control channel | 5.2s | External `control-send` commands and JSONL replies |

The final encode uses H.264 CRF 19 for MP4 and VP9 CRF 30 for WebM so UI text
stays legible in the hero slot. `hero-poster.png` is extracted from the debugger
beat, which gives the page a mid-action still before playback starts.
The larger-file beat is generated inside the throwaway fixture repository, not
checked into the real project tree.

## Dependencies

`Xvfb`, a screen grabber — either ImageMagick (`import`/`convert`) **or** x11-apps
(`xwd`) plus Pillow — **`xdotool`** (live keystroke/typing injection — required for
the hero video and the welcome-tab close), and — for the debugger scene — `gdb`
plus the bundled `gdb-dap` plugin. The hero *video* additionally needs `ffmpeg`
(with libx264 + libvpx-vp9); `--shots-only` and the `tools/repro/` scripts do not.
On Debian/Ubuntu:

```bash
sudo apt install xvfb ffmpeg imagemagick xdotool gdb
# or, without ImageMagick (stills + repro scripts only):
sudo apt install xvfb x11-apps python3-pil xdotool gdb
```

## Troubleshooting

- **Leftover Xvfb on `:99`** — a crashed run can leave it. Clear with
  `for p in $(pgrep -x Xvfb); do kill "$p"; done; rm -f /tmp/.X99-lock`. Do **not**
  `pkill -f Xvfb`: `-f` can match an unrelated wrapper process.
- **`/tmp/.X11-unix` owner is not root** — Xvfb may fail before SDL starts with
  `Owner of /tmp/.X11-unix should be set to root`. Repair the socket directory:
  `sudo chown root:root /tmp/.X11-unix && sudo chmod 1777 /tmp/.X11-unix`.
- **Empty `adapters` / no debugger** — the `gdb-dap` plugin wasn't seeded; confirm
  `plugins/gdb-dap/` exists (lib.sh copies it into the isolated config).
- **A beat misfires in the hero video** — timing is sleep-based; just re-run.
  Review the output before committing.
