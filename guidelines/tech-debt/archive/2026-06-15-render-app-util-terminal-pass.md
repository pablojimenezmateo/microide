# 2026-06-15 deep pass: render / app / util / terminal (§25)

- Date: 2026-06-15
- Area: rendering, app, util, terminal
- Source: §25

## Summary

The four subsystems that had only received narrow single-purpose fixes got the same
dedup/tech-debt/correctness/footprint treatment as the larger subsystems, grouped into four efforts.

## Resolution — fixed in this pass (with regression coverage)

- **util shared primitives**: new `util/Hex.h` (`HexDigitValue`/`ParseHexByte`/`DecodeHexColor`/
  `PercentDecode`) replaces four hand-rolled hex/percent decoders across `JsonValue`, `FileUri`,
  terminal OSC-7, and theme/project colour parsers (removing `strtol`/`atoi`). Added `util::AppendUtf8`
  + `kUtf8ReplacementChar` (dedup'd JSON's encoder and four terminal replacement-char literals),
  `SplitAsciiWhitespace`, `IsAllAsciiDigits`, and made `DecodeLines` single-pass. Tests in
  `StringUtilTests`. (The suspected trailing-`%XX` percent-decode off-by-one was a false alarm:
  `i+2 < size` ≡ `i+3 <= size`; regression test pins the case.)
- **render theme/text**: extracted the byte-identical ANSI palette into `render/AnsiPalette` (shared by
  theme + terminal; parity test locks them). Decomposed `Theme.cpp` into `render/ColorMath`
  (WCAG/blend) + `render/ThemeFile` (`.microide` parser) + the derivation/discovery left in
  `Theme.cpp`. Removed the dead background path from `SdlTtfTextBackend`'s texture cache. Tests in
  `ThemeTests`.
- **app event loop**: extracted `RedrawTraceAccumulator` (testable bookkeeping) and
  `SceneTexturePresenter` (RAII texture + resize coalescing) out of `Application`; extracted a pure,
  tested `ChooseIdleWait` policy; made the partial-redraw trace label lazy; flag-gated `quick_exit`
  via `AppStartupOptions`. The resize-coalescing architecture-lint invariant was repointed at
  `SceneTexturePresenter`. Tests in `ApplicationTests`; render path verified via a live ASAN app run.
- **terminal escape correctness** (real bug): DCS/APC/PM/SOS payloads (`ESC P/X/^/_`) leaked onto the
  grid (Sixel, Kitty graphics, tmux passthrough, DECRQSS); added `EscapeMode::StringPayload` that
  consumes them to ST/BEL. Bounded the escape buffers (8 KB cap, abandon-on-overflow,
  `TerminalEscapeSequencesAborted` counter). Replaced `std::atoi` (overflow UB) in the CSI/SGR parser
  and OSC-4 query with clamped `ParseInt64`. New `TerminalSessionTests` + self-contained
  `TerminalCsiParserFuzz` (728k iters clean under ASAN).

Validated: full `ctest` green (incl. ArchitectureInvariants); ASAN and UBSAN presets green;
CSI-parser fuzzer clean; the app launches and renders. The deferred items (R5a glyph-cell atlas; T3,
T5a, terminal output fuzzer; A2 headless lifecycle) were all closed on 2026-06-16 — see
`2026-06-16-terminal-headless-and-glyph-atlas-closeout.md`.
