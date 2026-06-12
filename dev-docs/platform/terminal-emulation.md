# Terminal Emulation

This documents the terminal emulator's supported escape sequences, keyboard
protocols, and the performance posture of the grid model. The emulator lives in
`src/terminal/` (the host-owned renderer is in
`src/workspace/WorkspaceShellRenderBottomPanel.cpp`).

The driving goal is to run modern interactive TUI applications (Claude CLI,
Codex CLI, vim, less, tmux) correctly while staying fast and low-footprint.

## Cell model

`TerminalCell` (`src/terminal/TerminalCell.h`) is a trivially copyable POD so
snapshots and scrollback trims are bulk `memcpy`s:

- 4-byte inline UTF-8 glyph + length byte (combining marks fold into the base
  cell up to the 4-byte limit).
- `TerminalStyle`: optional fg/bg `SDL_Color` plus a `uint16_t attrs` bitfield
  (`cell_attr::*`) carrying bold, dim, italic, underline, double-underline,
  blink, inverse, hidden, strikethrough, and the wide-trailing marker.

`sizeof(TerminalCell) == 18` bytes. Replacing the two old `bool` flags with the
bitfield costs one byte of alignment padding but adds the full SGR attribute set
and double-width support for free.

### Wide characters

`util::CodepointDisplayWidth` (a `wcwidth`-style table matching the layout
assumptions of `unicode-width` / `string-width`) classifies each glyph as width
0/1/2. A double-width glyph occupies a lead cell plus a `kWideTrailing` spacer
cell that carries the lead's style; the renderer skips the spacer so the glyph
paints across both columns and grid alignment with the application holds.

## Supported sequences

### SGR (`CSI … m`)

Reset (0), bold (1), dim (2), italic (3), underline (4 incl. `4:0/4:2` colon
sub-parameters), blink (5/6), inverse (7), hidden (8), strikethrough (9), double
underline (21), and their resets (22–29). 16-color (30–37/40–47/90–97/100–107),
256-color (`38;5`/`48;5`), and truecolor in both legacy (`38;2;r;g;b`) and ITU
colon (`38:2:r:g:b`, `38:2::r:g:b`) forms. Underline color (58/59) is parsed and
ignored; overline (53/55) is accepted and ignored.

### Cursor / editing / scrolling

Cursor motion `A B C D E F G H f d`, ED/EL `J K`, insert/delete line `L M`,
delete/erase/insert char `P X @`, scroll `S T`, scroll region `r` (DECSTBM),
save/restore cursor (`s u`, `ESC 7/8`), origin/autowrap modes, alternate screen
(47/1047/1048/1049).

### Tab stops

Real per-column tab stops: HTS (`ESC H`), TBC (`CSI g`), CHT (`CSI I`), CBT
(`CSI Z`); `\t` honors them. Rebuilt to the default 8-column grid on resize.

### Modes & reports

- DA / DA2 (`CSI c`, `CSI > c`), DSR cursor position & status (`CSI 5n/6n`).
- DECRQM (`CSI ? … $ p`) → `CSI ? mode ; state $ y` for every tracked private
  mode, so applications detect feature support without timing out.
- Synchronized output (DEC 2026): tracked, and the host coalesces redraws across
  the open frame (see Performance).
- Cursor shape DECSCUSR (`CSI … SP q`) and blink (mode 12) tracked and exposed.
- Mouse tracking 1000/1002/1003 + SGR 1006, focus events 1004, bracketed paste
  2004.

### OSC

- 0/1/2 window title.
- 7 working directory (`file://host/path`, percent-decoded), exposed via
  `reported_working_directory()`.
- 52 clipboard (read/write).
- 10/11/12 and 4 color queries are answered with `rgb:` replies (so background
  light/dark detection works); 8/9/104/110–112/133 are accepted and ignored.

### Keyboard

Input is encoded by `FormatTerminalKeyPress`
(`src/terminal/TerminalSessionInputEncoding.cpp`):

- Legacy xterm: modified arrows/nav/`F1`–`F12` via `CSI 1;mod letter` and
  `CSI n;mod ~`, application-cursor SS3 forms, `CSI Z` for Shift+Tab, C0 control
  bytes for Ctrl combos, ESC-prefixed Alt combos.
- Kitty keyboard protocol: query (`CSI ? u`), push (`CSI > flags u`), pop
  (`CSI < n u`), set (`CSI = flags ; mode u`). While active, keys the legacy
  encoding cannot disambiguate (modified Enter/Tab/Backspace/Escape, Ctrl
  letters) are emitted in CSI-u form — this is what lets Claude/Codex bind
  Shift+Enter and similar.

Plain printable text continues to flow through `SDL_EVENT_TEXT_INPUT`, which is
correct for the common Kitty flag set (disambiguate / report-event-types) where
unmodified text is not escaped.

The PTY child sees `TERM=xterm-256color` and `COLORTERM=truecolor`.

## Performance

Goals (per repo priority): correctness first, then speed, then low CPU/memory.

- **Hot path stays branch-cheap.** Printable ASCII bypasses UTF-8 decoding and
  the width tables (`PutGlyphLocked` / `CodepointDisplayWidth` fast paths), so
  the per-byte output loop cost is unchanged for plain output.
- **Synchronized-output coalescing.** Under DEC 2026 the redraw wake is
  suppressed until the frame closes (with an 8-batch safety cap), so a
  multi-write frame repaints once instead of per write — fewer paints, less CPU,
  no tearing.
- **Trivially copyable cells.** Snapshots and scrollback trims remain bulk
  `memcpy`/`erase` over POD; no per-cell heap traffic was introduced.
- **Allocation discipline.** SGR colon-parsing allocates only on actual SGR
  sequences (rare relative to glyph output); the streaming-glyph path allocates
  nothing beyond line growth.

### Sanitizers

The terminal and string-utility suites pass clean under AddressSanitizer with
leak detection (`microide-asan` preset): 92 + 12 tests, no errors. The new
fixed-buffer writes (combining-mark fold, wide-glyph spacer) are bounds-checked
by construction.

### Perf-harness result (`terminal_scroll_long_output`)

Scenario streams `yes perf-output-line` and scrolls. The committed baseline is
measured on `perf-runner-v1`; local-advisory runs on other hardware are not
comparable to it (the fixed wall-window reads a machine-dependent amount of the
`yes` flood, so absolute counts scale with throughput). The valid signal is a
**same-machine new-vs-old** comparison, run here on the software renderer with
`SDL_VIDEODRIVER=dummy`, 8 iterations:

| metric            | pre-change | this change |
| ----------------- | ---------- | ----------- |
| p50 wall (ms)     | 96.4–98.0  | 94.0–95.4   |
| p50 allocations   | 12378      | 12378       |
| max allocations   | 25057      | 25058       |

Allocations are unchanged and wall time is within noise (marginally lower) — no
regression. The streaming-glyph hot path is allocation-neutral: the ASCII fast
paths keep per-character cost identical, and the new attribute/width/keyboard
work adds cost only on escape sequences and key presses, not on plain output.

Treat absolute numbers as microide-vs-itself regression signals only (see
`dev-docs/performance/perf-harness.md`); do not compare across machines.
