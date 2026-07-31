#!/usr/bin/env python3
"""xwd2png.py — convert an X Window Dump (xwd) stream to PNG.

The capture pipeline's preferred grabber is ImageMagick's `import`, but
ImageMagick is a heavy dependency that is frequently absent (and is being
dropped from several distros' default images). `xwd` ships with x11-apps, is
tiny, and is present anywhere Xvfb is. This converter is the fallback that lets
the whole media/repro pipeline run with only `xwd` + Pillow.

Usage:
    xwd -display :99 -root -silent | tools/capture-media/xwd2png.py out.png [--half]

`--half` downsamples 2x (the HiDPI deliverable path, replacing `convert -resize 50%`).
"""

import struct
import sys

from PIL import Image

# 25 big-endian uint32 fields; see X11/XWDFile.h.
_HEADER_FIELDS = (
    "header_size file_version pixmap_format pixmap_depth pixmap_width "
    "pixmap_height xoffset byte_order bitmap_unit bitmap_bit_order bitmap_pad "
    "bits_per_pixel bytes_per_line visual_class red_mask green_mask blue_mask "
    "bits_per_rgb colormap_entries ncolors window_width window_height "
    "window_x window_y window_bdrwidth"
).split()

# (bits_per_pixel, byte_order, red_mask, green_mask, blue_mask) -> PIL rawmode.
# byte_order 0 is LSBFirst, 1 is MSBFirst.
_RAW_MODES = {
    (32, 0, 0xFF0000, 0xFF00, 0xFF): "BGRX",
    (32, 1, 0xFF0000, 0xFF00, 0xFF): "XRGB",
    (32, 0, 0xFF, 0xFF00, 0xFF0000): "RGBX",
    (32, 1, 0xFF, 0xFF00, 0xFF0000): "XBGR",
    (24, 0, 0xFF0000, 0xFF00, 0xFF): "BGR",
    (24, 1, 0xFF0000, 0xFF00, 0xFF): "RGB",
}


def _mask_shift_scale(mask):
    """Return (shift, max) so (pixel & mask) >> shift lands in [0, max]."""
    if mask == 0:
        return 0, 1
    shift = (mask & -mask).bit_length() - 1
    return shift, mask >> shift


def _decode_slow(data, hdr):
    """Per-pixel decode for visuals the rawmode table does not cover."""
    width, height = hdr["pixmap_width"], hdr["pixmap_height"]
    stride, bpp = hdr["bytes_per_line"], hdr["bits_per_pixel"]
    order = "big" if hdr["byte_order"] == 1 else "little"
    nbytes = (bpp + 7) // 8
    channels = []
    for name in ("red_mask", "green_mask", "blue_mask"):
        shift, top = _mask_shift_scale(hdr[name])
        channels.append((hdr[name], shift, top or 1))
    out = bytearray(width * height * 3)
    o = 0
    for y in range(height):
        row = data[y * stride : y * stride + width * nbytes]
        for x in range(width):
            pixel = int.from_bytes(row[x * nbytes : (x + 1) * nbytes], order)
            for mask, shift, top in channels:
                out[o] = ((pixel & mask) >> shift) * 255 // top
                o += 1
    return Image.frombytes("RGB", (width, height), bytes(out))


def convert(stream):
    head = stream.read(100)
    if len(head) < 100:
        raise SystemExit("xwd2png: short read; is this an xwd stream?")
    hdr = dict(zip(_HEADER_FIELDS, struct.unpack(">25I", head)))
    if hdr["file_version"] != 7:
        raise SystemExit(f"xwd2png: unsupported xwd version {hdr['file_version']}")
    if hdr["pixmap_format"] != 2:
        raise SystemExit("xwd2png: only ZPixmap dumps are supported")
    # Skip the window-name tail of the header, then the colormap.
    stream.read(max(0, hdr["header_size"] - 100))
    stream.read(hdr["ncolors"] * 12)
    data = stream.read()

    width, height = hdr["pixmap_width"], hdr["pixmap_height"]
    stride = hdr["bytes_per_line"]
    if len(data) < stride * height:
        raise SystemExit(
            f"xwd2png: truncated image ({len(data)} bytes, need {stride * height})"
        )
    key = (
        hdr["bits_per_pixel"],
        hdr["byte_order"],
        hdr["red_mask"],
        hdr["green_mask"],
        hdr["blue_mask"],
    )
    rawmode = _RAW_MODES.get(key)
    if rawmode is None:
        return _decode_slow(data, hdr)
    return Image.frombuffer("RGB", (width, height), data, "raw", rawmode, stride, 1)


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    half = "--half" in argv[1:]
    if len(args) != 1:
        raise SystemExit(__doc__)
    image = convert(sys.stdin.buffer)
    if half:
        image = image.resize(
            (image.width // 2, image.height // 2), Image.Resampling.LANCZOS
        )
    image.save(args[0])
    print(f"{image.width}x{image.height}")


if __name__ == "__main__":
    main(sys.argv)
