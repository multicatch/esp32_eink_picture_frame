#!/usr/bin/env python3

"""
Convert an 800x480 24-bit BMP to 4bpp PE6DIB-style raw image data.

Input:
    BMP, already converted/dithered to the target Spectra 6 palette.

Output:
    192000-byte raw 4bpp image:
      - 2 pixels per byte
      - first pixel in high nibble
      - second pixel in low nibble

The palette/index mapping is deliberately configurable. Use --map to
specify RGB -> nibble mapping once we have a reference PE6DIB file.

Example:
    python bmp_to_4bpp.py input.bmp output.bin

Optional PE6DIB header:
    python bmp_to_4bpp.py input.bmp output.bin --header header.bin
"""

import sys
import argparse
import struct
from pathlib import Path
from PIL import Image


WIDTH = 800
HEIGHT = 480

# Default mapping. Replace these after comparing with a manufacturer BIN.
# RGB -> 4-bit value.
DEFAULT_PALETTE = {
    (0, 0, 0): 0x0,           # black
    (255, 255, 255): 0x1,     # white
    (0, 255, 0): 0x2,         # green
    (0, 0, 255): 0x3,         # blue
    (255, 0, 0): 0x4,         # red
    (255, 255, 0): 0x5,       # yellow
    (255, 165, 0): 0x6,       # orange
}


def read_bmp(path: Path) -> Image.Image:
    image = Image.open(path)

    if image.size != (WIDTH, HEIGHT):
        raise ValueError(
            f"Expected {WIDTH}x{HEIGHT}, got {image.width}x{image.height}"
        )

    return image.convert("RGB")


def build_palette(image: Image.Image):
    """
    Return RGB -> nibble mapping.

    If the BMP contains exactly the expected 7 colors, this is enough.
    We deliberately require exact palette colors instead of silently
    choosing the nearest color: the BMP is assumed to have already been
    converted/dithered by the user's existing converter.
    """
    colors = set(image.getdata())
    unknown = colors - set(DEFAULT_PALETTE)

    if unknown:
        sample = ", ".join(map(str, list(unknown)[:10]))
        raise ValueError(
            "BMP contains colors not present in the configured palette. "
            f"Examples: {sample}\n"
            "If your source BMP uses different RGB values, edit "
            "DEFAULT_PALETTE or provide a matching palette."
        )

    return DEFAULT_PALETTE


def convert(image: Image.Image, palette: dict[tuple[int, int, int], int]) -> bytes:
    pixels = image.load()
    output = bytearray(WIDTH * HEIGHT // 2)

    out = 0

    for y in range(HEIGHT):
        for x in range(0, WIDTH, 2):
            p0 = palette[pixels[x, y]]
            p1 = palette[pixels[x + 1, y]]

            output[out] = (p0 << 4) | p1
            out += 1

    return bytes(output)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument(
        "--header",
        type=Path,
        help="Optional 18-byte PE6DIB header to prepend."
    )
    args = parser.parse_args()

    image = read_bmp(args.input)
    palette = build_palette(image)
    data = convert(image, palette)

    if len(data) != WIDTH * HEIGHT // 2:
        raise AssertionError("Internal error: wrong output size")

    if args.header:
        header = args.header.read_bytes()
        if len(header) != 18:
            raise ValueError(
                f"PE6DIB header must be exactly 18 bytes, got {len(header)}"
            )
        data = header + data

    args.output.write_bytes(data)

    print(f"Input : {args.input}")
    print(f"Output: {args.output}")
    print(f"Image : {WIDTH}x{HEIGHT}, 4bpp")
    print(f"Data  : {WIDTH * HEIGHT // 2} bytes")
    print(f"Total : {len(data)} bytes")


if __name__ == "__main__":
    main()
