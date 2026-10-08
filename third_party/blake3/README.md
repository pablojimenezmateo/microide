# BLAKE3 (vendored)

The official C implementation from https://github.com/BLAKE3-team/BLAKE3, tag
`1.5.4`, directory `c/`: `blake3.{h,c}`, `blake3_impl.h`, `blake3_dispatch.c`,
`blake3_portable.c` and the intrinsics backends (`blake3_sse2.c`, `blake3_sse41.c`,
`blake3_avx2.c`, `blake3_avx512.c`, `blake3_neon.c`). Unmodified. Licensed CC0-1.0
or Apache-2.0 (`LICENSE_CC0`, `LICENSE_A2`).

Built from the C intrinsics with runtime CPU dispatch, never from the per-target
assembly, so a static server build needs no assembler per architecture. Used only
through `src/util/ContentHash.{h,cpp}`.
