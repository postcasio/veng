#include <support/GpuContext.h>

#include <cstdio>

namespace Veng::Test
{
    bool PixelsMatch(const std::span<const u8> pixels, const std::array<u8, 4>& expected)
    {
        if (pixels.empty() || pixels.size() % 4 != 0)
        {
            std::fprintf(stderr, "FAIL: unexpected pixel buffer size %zu\n", pixels.size());
            return false;
        }

        for (size_t p = 0; p < pixels.size(); p += 4)
        {
            for (u32 c = 0; c < 4; c++)
            {
                if (pixels[p + c] != expected[c])
                {
                    std::fprintf(stderr,
                                  "FAIL: pixel %zu channel %u = %u, expected %u\n",
                                  p / 4, c, pixels[p + c], expected[c]);
                    return false;
                }
            }
        }

        return true;
    }
}
