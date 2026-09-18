#!/usr/bin/env python3
"""Build-time gzip compressor for the WebUI assets embedded into the firmware.

The files under www/ stay plain text in the repository; this step writes the
gzipped blobs into the build directory and CMake embeds those instead
(see ../CMakeLists.txt). index.html + app.js + styles.css shrink from about
623 kB to about 123 kB of flash.

mtime is pinned to 0 so the compressed blob is byte-identical between builds
and an unchanged asset does not alter the firmware image.
"""

import gzip
import pathlib
import sys


def main(argv):
    if len(argv) != 3:
        print("usage: gzip_asset.py <source> <destination>", file=sys.stderr)
        return 2

    src = pathlib.Path(argv[1])
    dst = pathlib.Path(argv[2])

    data = src.read_bytes()
    blob = gzip.compress(data, compresslevel=9, mtime=0)
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_bytes(blob)

    ratio = (100 * len(blob)) // max(len(data), 1)
    print(f"gzip {src.name}: {len(data)} -> {len(blob)} B ({ratio}%)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
