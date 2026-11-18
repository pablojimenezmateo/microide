# SDL Vulkan renderer: constant-buffer overrun (upstream bug)

> Status: open upstream in SDL, **not reported, not patched, not worked around**.
> Found by the soak on 2026-11-18 (finding `67b2b7cd4e62`). Affects SDL 3.4.2
> (Ubuntu `libsdl3-0` 3.4.2+ds-1ubuntu1) and SDL `main` as of 2026-11-18.

## Symptom

microide died with SIGSEGV inside SDL during a frame (layout profile, 2560x1440,
lavapipe Vulkan):

```
VULKAN_SetDrawState          SDL_render_vulkan.c  (SDL_memcpy of the pixel-shader constants)
VULKAN_RunCommandQueue       SDL_render_vulkan.c:4020
FlushRenderCommands          SDL_render.c
SDL_RenderReadPixels         SDL_render.c
RedrawVerifier::Verify       src/app/RedrawVerifier.h:91
Application::Render          src/app/Application.cpp
```

The crashing instruction is the second 16-byte store of a 48-byte copy: the first
16 bytes landed, the next ones crossed into an unmapped page.

## Cause

`VULKAN_SetDrawState` uploads `VULKAN_PixelShaderConstants` (12 floats, 48 bytes)
into a persistently mapped uniform buffer of `SDL_VULKAN_CONSTANT_BUFFER_DEFAULT_SIZE`
(65536 bytes) whenever the constants change between draws. Each upload advances
the offset by 48 rounded up to `minUniformBufferOffsetAlignment`, and it rolls over
to a fresh buffer only when

```c
if (rendererData->currentConstantBufferOffset >= SDL_VULKAN_CONSTANT_BUFFER_DEFAULT_SIZE)
```

It checks where the write *starts*, not where it *ends*. With an alignment of 16
(lavapipe; any device reporting <= 16) the stride is 48 and 65536 is not a
multiple of it: slot 1366 starts at offset 65520, passes the check, and the
48-byte `SDL_memcpy` writes 32 bytes past the end of the buffer. It repeats every
1366 constant changes within one command buffer (the offset resets per command
buffer). Devices whose alignment divides 65536 into whole slots (64, 256) never
hit it.

Whether it crashes depends on what follows the mapping: usually other heap data
is silently corrupted, occasionally the next page is unmapped and the process
gets SIGSEGV -- hence one soak crash in ~1000 jobs.

The correct check is `currentConstantBufferOffset + sizeof(VULKAN_PixelShaderConstants)
> SDL_VULKAN_CONSTANT_BUFFER_DEFAULT_SIZE`.

## When microide reaches it

- Only with SDL's **Vulkan** renderer. `SDL_CreateRenderer(window, nullptr)` tries
  OpenGL first, so a normal desktop session does not use this path. The soak's
  Xvfb display has no usable GL, so SDL falls back to Vulkan on lavapipe
  (`microide render: SDL renderer driver='vulkan'` in the log).
- A frame needs > 1365 draws whose pixel-shader constants differ from the previous
  draw, e.g. alternating solid fills and textured glyph runs. A large window
  (2560x1440) with dense text gets there; the QA redraw verifier
  (`MICROIDE_QA_VERIFY_REDRAW=1`) renders each frame twice into one command buffer
  before `SDL_RenderReadPixels`, which doubles the count.

## Reproducer

[`sdl-vulkan-overrun/repro.c`](sdl-vulkan-overrun/repro.c) draws 3000 alternating
fills and textured quads into a target texture and reads it back.
[`sdl-vulkan-overrun/g.gdb`](sdl-vulkan-overrun/g.gdb) breaks on the copy
(`VULKAN_SetDrawState+759` in Ubuntu's 3.4.2 build) and reports each write whose
end passes 65536:

```bash
cc -O0 -g repro.c -o repro $(pkg-config --cflags --libs sdl3)
DEBUGINFOD_URLS=https://debuginfod.ubuntu.com xvfb-run -a gdb -batch -x g.gdb ./repro
# OVERRUN write at offset 65520 (+48 > 65536), write #1366
# OVERRUN write at offset 65520 (+48 > 65536), write #2732
```

The breakpoint offset and the `$rcx` (offset) register are specific to that build;
re-derive them with `disassemble VULKAN_SetDrawState` on another one.

## Triage note for the soak

A SIGSEGV whose stack ends in `VULKAN_SetDrawState` under `SDL_RenderReadPixels`
or `SDL_RenderPresent` on lavapipe is this bug, not microide's: annotate the
finding with a link to this page rather than chasing it.
