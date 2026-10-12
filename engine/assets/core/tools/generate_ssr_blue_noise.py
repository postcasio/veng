#!/usr/bin/env python3
"""Extracts the screen-space reflections' blue-noise sampler tables into data/ssr_blue_noise.bin.

The tables are the Owen-scrambled Sobol sequence and the 128x128 scrambling tile of Heitz and
Belcour's "A Low-Discrepancy Sampler that Distributes Monte Carlo Errors as a Blue Noise in Screen
Space" (2019), as the FidelityFX SDK v1.1.4 ships them in
sdk/src/components/sssr/samplerBlueNoiseErrorDistribution_128x128_OptimizedFor_2d2d2d2d_1spp.cpp
under AMD's MIT notice, reproduced below. That is the variant the SDK's SSSR host includes.

The output is the two tables back to back, one byte per entry, in the order the source lists them:
    sobol_256spp_256d    256 * 256 entries  (row-major 256x256, R32_UINT on the GPU)
    scramblingTile       128 * 128 * 8      (row-major 512x256, R32_UINT on the GPU)
Every entry is a byte value; the renderer widens each to 32 bits when it uploads the two textures.

Usage:
    python3 -I generate_ssr_blue_noise.py <path to the 1spp .cpp> [output .bin]

The output defaults to ../data/ssr_blue_noise.bin beside this script.

-----------------------------------------------------------------------------------------------
This file is part of the FidelityFX SDK.

Copyright (C) 2024 Advanced Micro Devices, Inc.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files(the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions :

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
"""

import pathlib
import re
import sys

TABLES = (
    ("sobol_256spp_256d", 256 * 256),
    ("scramblingTile", 128 * 128 * 8),
)


def extract(source: str, name: str, count: int) -> bytes:
    match = re.search(r"\b" + re.escape(name) + r"\s*\[[^\]]*\]\s*=\s*\{([^}]*)\}", source)
    if match is None:
        sys.exit(f"error: table {name} not found")
    values = [int(token) for token in match.group(1).split(",") if token.strip()]
    if len(values) != count:
        sys.exit(f"error: table {name} has {len(values)} entries, expected {count}")
    if any(v < 0 or v > 255 for v in values):
        sys.exit(f"error: table {name} holds a value outside one byte")
    return bytes(values)


def main() -> None:
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__.split("-----")[0].strip())
    source = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
    default_output = pathlib.Path(__file__).resolve().parent.parent / "data" / "ssr_blue_noise.bin"
    output = pathlib.Path(sys.argv[2]) if len(sys.argv) == 3 else default_output
    blob = b"".join(extract(source, name, count) for name, count in TABLES)
    output.write_bytes(blob)
    print(f"wrote {len(blob)} bytes to {output}")


if __name__ == "__main__":
    main()
