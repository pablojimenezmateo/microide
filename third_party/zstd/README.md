# zstd (vendored)

The official zstd library from https://github.com/facebook/zstd, release `v1.5.7`
(`zstd-1.5.7.tar.gz`, sha256
`eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3`, as published
beside the release): `lib/zstd.h`, `lib/zstd_errors.h` and the `lib/common`,
`lib/compress` and `lib/decompress` directories. Unmodified, except that
`decompress/huf_decompress_amd64.S` is left out: the library is built with
`ZSTD_DISABLE_ASM` from its portable C, so a static server build needs no
assembler per architecture (as for blake3). No dictionary builder, no legacy
formats, no multithreading. Dual-licensed BSD (`LICENSE`) or GPLv2 (`COPYING`).

Used only through `src/util/Zstd.{h,cpp}` for the remote mirror's
`--patch-from`-style deltas (dev-docs/design/remote-projects.md § 6.2).
