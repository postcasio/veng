#!/usr/bin/env python3
"""Generate the stochastic reflection chain's blue-noise tile (data/ssr_blue_noise.bin).

The tile is 128x128 texels of two eight-bit channels, interleaved (RG8, row-major), each channel an
independent void-and-cluster rank map (Ulichney, "The void-and-cluster method for dither array
generation", 1993): every channel value 0..255 occurs exactly 64 times, and neighbouring texels hold
values far apart, so any small window of the tile samples [0, 1) evenly. The shader rotates the
tile per frame (SsrBlueNoise, Veng/ssr_sample.slang).

The output is a pure function of the seeds below: the same Python produces the same bytes. Usage:

    python3 ssr_blue_noise.py [out.bin]

with the default output beside the core pack's other data.
"""
import math
import os
import random
import sys

SIZE = 128
COUNT = SIZE * SIZE
# The energy filter's spread, in texels (Ulichney's 1.5), and the radius past which it is dropped.
SIGMA = 1.5
RADIUS = 7
# The share of the initial pattern's texels set before relaxation.
INITIAL_FRACTION = 0.1
# One seed per channel, so the channels are independent.
SEEDS = (0x5EED0001, 0x5EED0002)

KERNEL = [
    (dx, dy, math.exp(-(dx * dx + dy * dy) / (2.0 * SIGMA * SIGMA)))
    for dy in range(-RADIUS, RADIUS + 1)
    for dx in range(-RADIUS, RADIUS + 1)
    if dx * dx + dy * dy <= RADIUS * RADIUS
]


class Pattern:
    """A binary pattern on the torus with each texel's filtered energy, split by state.

    `ones` holds the energy at set texels (minus infinity elsewhere) and `zeros` at clear ones (plus
    infinity elsewhere), so the tightest cluster is max(ones) and the largest void min(zeros).
    """

    def __init__(self, bits):
        self.bits = list(bits)
        energy = [0.0] * COUNT
        for index, bit in enumerate(self.bits):
            if bit:
                Splat(energy, index, 1.0)
        self.ones = [energy[i] if self.bits[i] else -math.inf for i in range(COUNT)]
        self.zeros = [energy[i] if not self.bits[i] else math.inf for i in range(COUNT)]

    def Set(self, index, value):
        sign = 1.0 if value else -1.0
        x, y = index % SIZE, index // SIZE
        ones, zeros, bits = self.ones, self.zeros, self.bits
        for dx, dy, weight in KERNEL:
            target = ((y + dy) % SIZE) * SIZE + (x + dx) % SIZE
            if bits[target]:
                ones[target] += sign * weight
            else:
                zeros[target] += sign * weight
        # Move this texel's energy to the other state's array.
        if value:
            ones[index], zeros[index] = zeros[index], math.inf
        else:
            zeros[index], ones[index] = ones[index], -math.inf
        bits[index] = value

    def TightestCluster(self):
        return self.ones.index(max(self.ones))

    def LargestVoid(self):
        return self.zeros.index(min(self.zeros))


def Splat(energy, index, weight):
    x, y = index % SIZE, index // SIZE
    for dx, dy, w in KERNEL:
        energy[((y + dy) % SIZE) * SIZE + (x + dx) % SIZE] += weight * w


def RankMap(seed):
    """Every texel's rank, 0..COUNT-1, in void-and-cluster order."""
    rng = random.Random(seed)
    initial = [False] * COUNT
    for index in rng.sample(range(COUNT), int(COUNT * INITIAL_FRACTION)):
        initial[index] = True

    # Relax the initial pattern: move the tightest cluster's texel to the largest void until the
    # move would put it back where it was.
    pattern = Pattern(initial)
    while True:
        cluster = pattern.TightestCluster()
        pattern.Set(cluster, False)
        void = pattern.LargestVoid()
        if void == cluster:
            pattern.Set(cluster, True)
            break
        pattern.Set(void, True)
    prototype = list(pattern.bits)
    ones = sum(prototype)

    rank = [0] * COUNT
    # Phase 1: rank the prototype's texels by removing the tightest cluster first.
    pattern = Pattern(prototype)
    for remaining in range(ones - 1, -1, -1):
        cluster = pattern.TightestCluster()
        pattern.Set(cluster, False)
        rank[cluster] = remaining
    # Phases 2 and 3: fill the largest void until the tile is full. On the torus the largest void
    # among clear texels is also the tightest cluster of clear texels, so one rule covers both.
    pattern = Pattern(prototype)
    for filled in range(ones, COUNT):
        void = pattern.LargestVoid()
        pattern.Set(void, True)
        rank[void] = filled
    return rank


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "..", "data", "ssr_blue_noise.bin")
    channels = [RankMap(seed) for seed in SEEDS]
    data = bytearray(COUNT * 2)
    for index in range(COUNT):
        for channel, ranks in enumerate(channels):
            data[index * 2 + channel] = ranks[index] * 256 // COUNT
    with open(out, "wb") as f:
        f.write(bytes(data))


if __name__ == "__main__":
    main()
